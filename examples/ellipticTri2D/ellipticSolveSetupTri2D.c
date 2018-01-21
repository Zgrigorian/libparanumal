#include "ellipticTri2D.h"

solver_t *ellipticSolveSetupTri2D(mesh_t *mesh, dfloat tau, dfloat lambda, iint *BCType,
                      occa::kernelInfo &kernelInfo, const char *options, const char *parAlmondOptions){

  iint Ntotal = mesh->Np*mesh->Nelements;
  iint Nblock = (Ntotal+blockSize-1)/blockSize;
  iint Nhalo = mesh->Np*mesh->totalHaloPairs;
  iint Nall   = Ntotal + Nhalo;

  solver_t *solver = (solver_t*) calloc(1, sizeof(solver_t));

  solver->tau = tau;

  solver->mesh = mesh;

  solver->BCType = BCType;

  solver->p   = (dfloat*) calloc(Nall,   sizeof(dfloat));
  solver->z   = (dfloat*) calloc(Nall,   sizeof(dfloat));
  solver->Ax  = (dfloat*) calloc(Nall,   sizeof(dfloat));
  solver->Ap  = (dfloat*) calloc(Nall,   sizeof(dfloat));
  solver->tmp = (dfloat*) calloc(Nblock, sizeof(dfloat));

  solver->grad = (dfloat*) calloc(Nall*4, sizeof(dfloat));

  solver->o_p   = mesh->device.malloc(Nall*sizeof(dfloat), solver->p);
  solver->o_rtmp= mesh->device.malloc(Nall*sizeof(dfloat), solver->p);
  solver->o_z   = mesh->device.malloc(Nall*sizeof(dfloat), solver->z);

  solver->o_res = mesh->device.malloc(Nall*sizeof(dfloat), solver->z);
  solver->o_Sres = mesh->device.malloc(Nall*sizeof(dfloat), solver->z);
  solver->o_Ax  = mesh->device.malloc(Nall*sizeof(dfloat), solver->p);
  solver->o_Ap  = mesh->device.malloc(Nall*sizeof(dfloat), solver->Ap);
  solver->o_tmp = mesh->device.malloc(Nblock*sizeof(dfloat), solver->tmp);

  solver->o_grad  = mesh->device.malloc(Nall*4*sizeof(dfloat), solver->grad);

  //setup async halo stream
  solver->defaultStream = mesh->device.getStream();
  solver->dataStream = mesh->device.createStream();
  mesh->device.setStream(solver->defaultStream);

  iint Nbytes = mesh->totalHaloPairs*mesh->Np*sizeof(dfloat);
  if(Nbytes>0){
    occa::memory o_sendBuffer = mesh->device.mappedAlloc(Nbytes, NULL);
    occa::memory o_recvBuffer = mesh->device.mappedAlloc(Nbytes, NULL);

    solver->sendBuffer = (dfloat*) o_sendBuffer.getMappedPointer();
    solver->recvBuffer = (dfloat*) o_recvBuffer.getMappedPointer();

    occa::memory o_gradSendBuffer = mesh->device.mappedAlloc(2*Nbytes, NULL);
    occa::memory o_gradRecvBuffer = mesh->device.mappedAlloc(2*Nbytes, NULL);

    solver->gradSendBuffer = (dfloat*) o_gradSendBuffer.getMappedPointer();
    solver->gradRecvBuffer = (dfloat*) o_gradRecvBuffer.getMappedPointer();
  }else{
    solver->sendBuffer = NULL;
    solver->recvBuffer = NULL;
  }
  mesh->device.setStream(solver->defaultStream);

  solver->type = strdup(dfloatString);

  solver->Nblock = Nblock;

  //fill geometric factors in halo
  if(mesh->totalHaloPairs){
    iint Nlocal = mesh->Nelements*mesh->Np;
    iint Nhalo  = mesh->totalHaloPairs*mesh->Np;
    dfloat *vgeoSendBuffer = (dfloat*) calloc(mesh->totalHaloPairs*mesh->Nvgeo, sizeof(dfloat));

    // import geometric factors from halo elements
    mesh->vgeo = (dfloat*) realloc(mesh->vgeo, (mesh->Nelements+mesh->totalHaloPairs)*mesh->Nvgeo*sizeof(dfloat));

    meshHaloExchange(mesh,
         mesh->Nvgeo*sizeof(dfloat),
         mesh->vgeo,
         vgeoSendBuffer,
         mesh->vgeo + mesh->Nelements*mesh->Nvgeo);

    mesh->o_vgeo =
      mesh->device.malloc((mesh->Nelements + mesh->totalHaloPairs)*mesh->Nvgeo*sizeof(dfloat), mesh->vgeo);
  }

  //set up memory for linear solver
  if(strstr(options,"GMRES")) {
    solver->GMRESrestartFreq = 20;
    printf("GMRES Restart Frequency = %d \n", solver->GMRESrestartFreq);
    solver->HH  = (dfloat*) calloc((solver->GMRESrestartFreq+1)*solver->GMRESrestartFreq,   sizeof(dfloat));
    
    solver->o_V = (occa::memory *) calloc(solver->GMRESrestartFreq+1,sizeof(occa::memory));
    for (int i=0; i<=solver->GMRESrestartFreq; ++i) {
      solver->o_V[i] = mesh->device.malloc(Nall*sizeof(dfloat), solver->z);
    }
  }

  //build inverse of mass matrix
  mesh->invMM = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
  for (int n=0;n<mesh->Np*mesh->Np;n++)
    mesh->invMM[n] = mesh->MM[n];
  matrixInverse(mesh->Np,mesh->invMM);


  //check all the bounaries for a Dirichlet
  bool allNeumann = (lambda==0) ? true :false;
  solver->allNeumannPenalty = 1;
  iint totalElements = 0;
  MPI_Allreduce(&(mesh->Nelements), &totalElements, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);
  solver->allNeumannScale = 1.0/sqrt(mesh->Np*totalElements);

  solver->EToB = (int *) calloc(mesh->Nelements*mesh->Nfaces,sizeof(int));
  for (iint e=0;e<mesh->Nelements;e++) {
    for (int f=0;f<mesh->Nfaces;f++) {
      int bc = mesh->EToB[e*mesh->Nfaces+f];
      if (bc>0) {
        int BC = BCType[bc]; //get the type of boundary
        solver->EToB[e*mesh->Nfaces+f] = BC; //record it
        if (BC!=2) allNeumann = false; //check if its a Dirchlet
      }
    }
  }
  MPI_Allreduce(&allNeumann, &(solver->allNeumann), 1, MPI::BOOL, MPI_LAND, MPI_COMM_WORLD);
  printf("allNeumann = %d \n", solver->allNeumann);

  //set surface mass matrix for continuous boundary conditions
  mesh->sMT = (dfloat *) calloc(mesh->Np*mesh->Nfaces*mesh->Nfp,sizeof(dfloat));
  for (iint n=0;n<mesh->Np;n++) {
    for (iint m=0;m<mesh->Nfp*mesh->Nfaces;m++) {
      dfloat MSnm = 0;
      for (int i=0;i<mesh->Np;i++){
        MSnm += mesh->MM[n+i*mesh->Np]*mesh->LIFT[m+i*mesh->Nfp*mesh->Nfaces];
      }
      mesh->sMT[n+m*mesh->Np]  = MSnm;
    }
  }
  mesh->o_sMT = mesh->device.malloc(mesh->Np*mesh->Nfaces*mesh->Nfp*sizeof(dfloat), mesh->sMT);

  /* sparse basis setup */
  //build inverse vandermonde matrix
  mesh->invSparseV = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
  for (int n=0;n<mesh->Np*mesh->Np;n++)
    mesh->invSparseV[n] = mesh->sparseV[n];
  matrixInverse(mesh->Np,mesh->invSparseV);

  int paddedRowSize = 4*((mesh->SparseNnzPerRow+3)/4); //make the nnz per row a multiple of 4

  char* IndTchar = (char*) calloc(paddedRowSize*mesh->Np,sizeof(char));
  for (int m=0;m<paddedRowSize/4;m++) {
    for (int n=0;n<mesh->Np;n++) {
      for (int k=0;k<4;k++) {
        if (k+4*m < mesh->SparseNnzPerRow) {
          IndTchar[k+4*n+m*4*mesh->Np] = mesh->sparseStackedNZ[n+(k+4*m)*mesh->Np];        
        } else {
          IndTchar[k+4*n+m*4*mesh->Np] = 0;
        }
      }
    }
  }
  
  mesh->o_sparseSrrT = mesh->device.malloc(mesh->Np*mesh->SparseNnzPerRow*sizeof(dfloat), mesh->sparseSrrT);
  mesh->o_sparseSrsT = mesh->device.malloc(mesh->Np*mesh->SparseNnzPerRow*sizeof(dfloat), mesh->sparseSrsT);
  mesh->o_sparseSssT = mesh->device.malloc(mesh->Np*mesh->SparseNnzPerRow*sizeof(dfloat), mesh->sparseSssT);
  
  mesh->o_sparseStackedNZ = mesh->device.malloc(mesh->Np*paddedRowSize*sizeof(char), IndTchar);
  free(IndTchar);

  //copy boundary flags
  solver->o_EToB = mesh->device.malloc(mesh->Nelements*mesh->Nfaces*sizeof(int), solver->EToB);

  //add standard boundary functions
  char *boundaryHeaderFileName;
  boundaryHeaderFileName = strdup(DHOLMES "/examples/ellipticTri2D/ellipticBoundary2D.h");
  kernelInfo.addInclude(boundaryHeaderFileName);

  kernelInfo.addParserFlag("automate-add-barriers", "disabled");

  if(mesh->device.mode()=="CUDA"){ // add backend compiler optimization for CUDA
    kernelInfo.addCompilerFlag("-Xptxas -dlcm=ca");
  }

  kernelInfo.addDefine("p_blockSize", blockSize);

  // add custom defines
  kernelInfo.addDefine("p_NpP", (mesh->Np+mesh->Nfp*mesh->Nfaces));
  kernelInfo.addDefine("p_Nverts", mesh->Nverts);
  kernelInfo.addDefine("p_SparseNnzPerRow", paddedRowSize);


  //sizes for the coarsen and prolongation kernels. degree N to degree 1
  kernelInfo.addDefine("p_NpFine", mesh->Np);
  kernelInfo.addDefine("p_NpCoarse", mesh->Nverts);

  kernelInfo.addDefine("p_NpFEM", mesh->NpFEM);

  int Nmax = mymax(mesh->Np, mesh->Nfaces*mesh->Nfp);
  kernelInfo.addDefine("p_Nmax", Nmax);

  int maxNodes = mymax(mesh->Np, (mesh->Nfp*mesh->Nfaces));
  kernelInfo.addDefine("p_maxNodes", maxNodes);

  int NblockV = 256/mesh->Np; // works for CUDA
  int NnodesV = 1; //hard coded for now
  kernelInfo.addDefine("p_NblockV", NblockV);
  kernelInfo.addDefine("p_NnodesV", NnodesV);

  int NblockS = 256/maxNodes; // works for CUDA
  kernelInfo.addDefine("p_NblockS", NblockS);

  int NblockP = 256/(4*mesh->Np); // get close to 256 threads
  kernelInfo.addDefine("p_NblockP", NblockP);

  int NblockG;
  if(mesh->Np<=32) NblockG = ( 32/mesh->Np );
  else NblockG = 256/mesh->Np;
  kernelInfo.addDefine("p_NblockG", NblockG);

  mesh->haloExtractKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/meshHaloExtract2D.okl",
				       "meshHaloExtract2D",
				       kernelInfo);

  mesh->gatherKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/gather.okl",
				       "gather",
				       kernelInfo);

  mesh->scatterKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/scatter.okl",
				       "scatter",
				       kernelInfo);

  mesh->gatherScatterKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/gatherScatter.okl",
               "gatherScatter",
               kernelInfo);

  mesh->getKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/get.okl",
				       "get",
				       kernelInfo);

  mesh->putKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/put.okl",
				       "put",
				       kernelInfo);

  mesh->sumKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/sum.okl",
               "sum",
               kernelInfo);

  mesh->addScalarKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/addScalar.okl",
               "addScalar",
               kernelInfo);

  mesh->maskKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/mask.okl",
               "mask",
               kernelInfo);

  if (strstr(options,"SPARSE")) {
    solver->AxKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxSparseTri2D.okl",
                 "ellipticAxTri2D_v0",
                 kernelInfo);

    solver->partialAxKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxSparseTri2D.okl",
                 "ellipticPartialAxTri2D_v0",
                 kernelInfo);
  } else {
    solver->AxKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxTri2D.okl",
                 "ellipticAxTri2D",
                 kernelInfo);

    solver->partialAxKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxTri2D.okl",
                 "ellipticPartialAxTri2D",
                 kernelInfo);
  }

  solver->weightedInnerProduct1Kernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/weightedInnerProduct1.okl",
				       "weightedInnerProduct1",
				       kernelInfo);

  solver->weightedInnerProduct2Kernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/weightedInnerProduct2.okl",
				       "weightedInnerProduct2",
				       kernelInfo);

  solver->innerProductKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/innerProduct.okl",
				       "innerProduct",
				       kernelInfo);

  solver->scaledAddKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/scaledAdd.okl",
					 "scaledAdd",
					 kernelInfo);

  solver->dotMultiplyKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/dotMultiply.okl",
					 "dotMultiply",
					 kernelInfo);

  solver->dotDivideKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/dotDivide.okl",
					 "dotDivide",
					 kernelInfo);

  if (strstr(options,"BERN")) {

    solver->gradientKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticGradientBBTri2D.okl",
               "ellipticGradientBBTri2D",
           kernelInfo);

    solver->partialGradientKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticGradientBBTri2D.okl",
                 "ellipticPartialGradientBBTri2D",
                  kernelInfo);

    solver->BRGradientVolumeKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayBBTri2D.okl",
                 "ellipticBBBRGradientVolume2D",
                 kernelInfo);

    solver->BRGradientSurfaceKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayBBTri2D.okl",
                 "ellipticBBBRGradientSurface2D",
                 kernelInfo);

    solver->BRDivergenceVolumeKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayBBTri2D.okl",
                 "ellipticBBBRDivergenceVolume2D",
                 kernelInfo);

    if (strstr(options,"NONSYM")) {
      solver->ipdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgBBNonSymTri2D.okl",
                   "ellipticAxIpdgBBNonSymTri2D",
                   kernelInfo);

      solver->partialIpdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgBBNonSymTri2D.okl",
                   "ellipticPartialAxIpdgBBNonSymTri2D",
                   kernelInfo);

      solver->BRDivergenceSurfaceKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayBBTri2D.okl",
                   "ellipticBBBRDivergenceSurfaceNonSym2D",
                   kernelInfo);
    } else {
      solver->ipdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgBBTri2D.okl",
                   "ellipticAxIpdgBBTri2D",
                   kernelInfo);

      solver->partialIpdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgBBTri2D.okl",
                   "ellipticPartialAxIpdgBBTri2D",
                   kernelInfo);

      solver->BRDivergenceSurfaceKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayBBTri2D.okl",
                   "ellipticBBBRDivergenceSurface2D",
                   kernelInfo);
    }
      
  } else if (strstr(options,"NODAL")) {

    solver->gradientKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticGradientTri2D.okl",
               "ellipticGradientTri2D_v0",
           kernelInfo);

    solver->partialGradientKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticGradientTri2D.okl",
             "ellipticPartialGradientTri2D_v0",
           kernelInfo);
               

    solver->BRGradientVolumeKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayTri2D.okl",
                 "ellipticBRGradientVolume2D",
                 kernelInfo);

    solver->BRGradientSurfaceKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayTri2D.okl",
                 "ellipticBRGradientSurface2D",
                 kernelInfo);

    solver->BRDivergenceVolumeKernel =
      mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayTri2D.okl",
                 "ellipticBRDivergenceVolume2D",
                 kernelInfo);

    if (strstr(options,"NONSYM")) {
      solver->ipdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgNonSymTri2D.okl",
                   "ellipticAxIpdgNonSymTri2D",
                   kernelInfo);

      solver->partialIpdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgNonSymTri2D.okl",
                   "ellipticPartialAxIpdgNonSymTri2D",
                   kernelInfo);

      solver->BRDivergenceSurfaceKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayTri2D.okl",
                   "ellipticBRDivergenceSurfaceNonSym2D",
                   kernelInfo);
    } else {
      solver->ipdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgTri2D.okl",
                   "ellipticAxIpdgTri2D",
                   kernelInfo);

      solver->partialIpdgKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticAxIpdgTri2D.okl",
                   "ellipticPartialAxIpdgTri2D",
                   kernelInfo);

      solver->BRDivergenceSurfaceKernel =
        mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBassiRebayTri2D.okl",
                   "ellipticBRDivergenceSurface2D",
                   kernelInfo);
    }
  }


  //on-host version of gather-scatter
  mesh->hostGsh = gsParallelGatherScatterSetup(mesh->Nelements*mesh->Np, mesh->globalIds);

  // setup occa gather scatter
  mesh->ogs = meshParallelGatherScatterSetup(mesh,Ntotal,
                                             mesh->gatherLocalIds,
                                             mesh->gatherBaseIds,
                                             mesh->gatherBaseRanks,
                                             mesh->gatherHaloFlags);
  solver->o_invDegree = mesh->ogs->o_invDegree;

  // set up separate gather scatter infrastructure for halo and non halo nodes
  ellipticParallelGatherScatterSetup(solver);

  //make a node-wise bc flag using the gsop (prioritize Dirichlet boundaries over Neumann)
  mesh->mapB = (int *) calloc(mesh->Nelements*mesh->Np,sizeof(int));
  for (iint e=0;e<mesh->Nelements;e++) {
    for (int n=0;n<mesh->Np;n++) mesh->mapB[n+e*mesh->Np] = 1E9;
    for (int f=0;f<mesh->Nfaces;f++) {
      int bc = mesh->EToB[f+e*mesh->Nfaces];
      if (bc>0) {
        int BCflag = BCType[bc]; //translate the mesh bc flag
        for (int n=0;n<mesh->Nfp;n++) {
          int fid = mesh->faceNodes[n+f*mesh->Nfp];
          mesh->mapB[fid+e*mesh->Np] = BCflag;
        }
      }
    }
  }
  gsParallelGatherScatter(mesh->hostGsh, mesh->mapB, "int", "min"); 

  //use the bc flags to find masked ids
  mesh->Nmasked = 0;
  for (iint n=0;n<mesh->Nelements*mesh->Np;n++) {
    if (mesh->mapB[n] == 1E9) {
      mesh->mapB[n] = 0.;
    } else if (mesh->mapB[n] == 1) { //Dirichlet boundary
      mesh->mapB[n] = 1.;
      mesh->Nmasked++;
    }
  }
  mesh->o_mapB = mesh->device.malloc(mesh->Nelements*mesh->Np*sizeof(int), mesh->mapB);
  
  mesh->maskIds = (iint *) calloc(mesh->Nmasked, sizeof(iint));
  mesh->Nmasked =0; //reset
  for (iint n=0;n<mesh->Nelements*mesh->Np;n++) {
    if (mesh->mapB[n] == 1) mesh->maskIds[mesh->Nmasked++] = n;
  }
  if (mesh->Nmasked) mesh->o_maskIds = mesh->device.malloc(mesh->Nmasked*sizeof(iint), mesh->maskIds);


  if (strstr(options,"SPARSE")) {
    // make the gs sign change array for flipped trace modes
    //TODO this is a hack that likely will need updating for MPI and/or 3D
    mesh->mapSgn = (dfloat *) calloc(mesh->Nelements*mesh->Np,sizeof(dfloat));
    for (iint n=0;n<mesh->Nelements*mesh->Np;n++) mesh->mapSgn[n] = 1;

    for (iint e=0;e<mesh->Nelements;e++) {
      for (int n=0;n<mesh->Nfaces*mesh->Nfp;n++) {
        iint id = n+e*mesh->Nfp*mesh->Nfaces;
        if (mesh->mmapS[id]==-1) { //sign flipped
          if (mesh->vmapP[id] <= mesh->vmapM[id]){ //flip only the higher index in the array
            mesh->mapSgn[mesh->vmapM[id]]= -1;
          } 
        } 
      }
    }
    mesh->o_mapSgn = mesh->device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat), mesh->mapSgn);
  }


  /*preconditioner setup */
  solver->precon = (precon_t*) calloc(1, sizeof(precon_t));

  #if 0
  solver->precon->overlappingPatchKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticOasPreconTri2D.okl",
               "ellipticOasPreconTri2D",
               kernelInfo);
  #endif

  solver->precon->restrictKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPreconRestrictTri2D.okl",
               "ellipticFooTri2D",
               kernelInfo);

  solver->precon->coarsenKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPreconCoarsen.okl",
				       "ellipticPreconCoarsen",
				       kernelInfo);

  solver->precon->prolongateKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPreconProlongate.okl",
				       "ellipticPreconProlongate",
				       kernelInfo);


  solver->precon->blockJacobiKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticBlockJacobiPreconTri2D.okl",
				       "ellipticBlockJacobiPreconTri2D",
				       kernelInfo);

  solver->precon->approxPatchSolverKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchSolver2D.okl",
               "ellipticApproxPatchSolver2D",
               kernelInfo);

  solver->precon->exactPatchSolverKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchSolver2D.okl",
               "ellipticExactPatchSolver2D",
               kernelInfo);

  solver->precon->patchGatherKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchGather.okl",
               "ellipticPatchGather",
               kernelInfo);

  solver->precon->approxFacePatchSolverKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchSolver2D.okl",
               "ellipticApproxFacePatchSolver2D",
               kernelInfo);

  solver->precon->exactFacePatchSolverKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchSolver2D.okl",
               "ellipticExactFacePatchSolver2D",
               kernelInfo);

  solver->precon->facePatchGatherKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchGather.okl",
               "ellipticFacePatchGather",
               kernelInfo);

  solver->precon->approxBlockJacobiSolverKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchSolver2D.okl",
               "ellipticApproxBlockJacobiSolver2D",
               kernelInfo);

  solver->precon->exactBlockJacobiSolverKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticPatchSolver2D.okl",
               "ellipticExactBlockJacobiSolver2D",
               kernelInfo);

  solver->precon->SEMFEMInterpKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticSEMFEMInterpTri2D.okl",
               "ellipticSEMFEMInterpTri2D",
               kernelInfo);

  solver->precon->SEMFEMAnterpKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/ellipticSEMFEMAnterpTri2D.okl",
               "ellipticSEMFEMAnterpTri2D",
               kernelInfo);

  long long int pre = mesh->device.memoryAllocated();

  occaTimerTic(mesh->device,"PreconditionerSetup");
  ellipticPreconditionerSetupTri2D(solver, solver->ogs, tau, lambda, BCType,  options, parAlmondOptions);
  occaTimerToc(mesh->device,"PreconditionerSetup");

  long long int usedBytes = mesh->device.memoryAllocated()-pre;

  solver->precon->preconBytes = usedBytes;

  return solver;
}

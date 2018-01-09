#include "boltzmannQuad3D.h"

solver_t *boltzmannSetupQuad3D(mesh_t *mesh){

  solver_t *solver = (solver_t*) calloc(1, sizeof(solver_t));

  solver->mesh = mesh;
  
  mesh->Nfields = 10;
  
  // compute samples of q at interpolation nodes
  mesh->q    = (dfloat*) calloc((mesh->totalHaloPairs+mesh->Nelements)*mesh->Np*mesh->Nfields,
				sizeof(dfloat));
  mesh->rhsq = (dfloat*) calloc(mesh->Nelements*mesh->Np*mesh->Nfields,
				sizeof(dfloat));
  mesh->resq = (dfloat*) calloc(mesh->Nelements*mesh->Np*mesh->Nfields,
				sizeof(dfloat));

  // set temperature, gas constant, wave speeds
  mesh->RT = 9.;
  mesh->sqrtRT = sqrt(mesh->RT);  
  
  // initial conditions
  // uniform flow
  dfloat rho = 1, u = 1, v = 0, w = 0; 
  dfloat sigma11 = 0, sigma12 = 0, sigma13 = 0, sigma22 = 0, sigma23 = 0, sigma33 = 0;

  //  dfloat ramp = 0.5*(1.f+tanh(10.f*(0-.5f)));
  dfloat q1bar = rho;
  dfloat q2bar = rho*u/mesh->sqrtRT;
  dfloat q3bar = rho*v/mesh->sqrtRT;
  dfloat q4bar = rho*w/mesh->sqrtRT;
  dfloat q5bar = (rho*u*u - sigma11)/(sqrt(2.)*mesh->RT);
  dfloat q6bar = (rho*v*v - sigma22)/(sqrt(2.)*mesh->RT);
  dfloat q7bar = (rho*w*w - sigma33)/(sqrt(2.)*mesh->RT);
  dfloat q8bar  = (rho*u*v - sigma12)/mesh->RT;
  dfloat q9bar =  (rho*u*w - sigma13)/mesh->RT;
  dfloat q10bar = (rho*v*w - sigma23)/mesh->RT;

  iint cnt = 0;
  for(iint e=0;e<mesh->Nelements;++e){
    for(iint n=0;n<mesh->Np;++n){
      dfloat t = 0;
      dfloat x = mesh->x[n + mesh->Np*e];
      dfloat y = mesh->y[n + mesh->Np*e];
      dfloat z = mesh->z[n + mesh->Np*e];

#if 0
      boltzmannCavitySolution2D(x, y, t,
				mesh->q+cnt, mesh->q+cnt+1, mesh->q+cnt+2);
#endif

#if 0
      boltzmannGaussianPulse2D(x, y, t,
			       mesh->q+cnt,
			       mesh->q+cnt+1,
			       mesh->q+cnt+2,
			       mesh->q+cnt+3,
			       mesh->q+cnt+4,
			       mesh->q+cnt+5);
#endif
      mesh->q[mesh->Np*cnt+0] = q1bar; // uniform density, zero flow

      mesh->q[mesh->Np*cnt+1] = q2bar;
      mesh->q[mesh->Np*cnt+2] = q3bar;
      mesh->q[mesh->Np*cnt+3] = q4bar;

      mesh->q[mesh->Np*cnt+4] = q5bar;
      mesh->q[mesh->Np*cnt+5] = q6bar;
      mesh->q[mesh->Np*cnt+6] = q7bar;

      mesh->q[mesh->Np*cnt+7] = q8bar;
      mesh->q[mesh->Np*cnt+8] = q9bar;
      mesh->q[mesh->Np*cnt+9] = q10bar;
      
      cnt += mesh->Nfields;
      
    }
  }
  // set BGK collision relaxation rate
  // nu = R*T*tau
  // 1/tau = RT/nu
  //  dfloat nu = 1.e-2/.5;
  //  dfloat nu = 1.e-3/.5;
  //  dfloat nu = 5.e-4;
  //    dfloat nu = 1.e-2; TW works for start up fence
  dfloat nu = 2.e-3;  // was 6.e-3
  mesh->tauInv = mesh->RT/nu; // TW
  
  // set penalty parameter
  //  mesh->Lambda2 = 0.5/(sqrt(3.)*mesh->sqrtRT);
  mesh->Lambda2 = 0.5/(mesh->sqrtRT);

  // set time step
  dfloat hmin = 1e9, hmax = 0;
  for(iint e=0;e<mesh->Nelements;++e){  

    for(iint f=0;f<mesh->Nfaces;++f){
      for(iint n=0;n<mesh->Nfp;++n){
	iint sid = mesh->Nsgeo*(mesh->Nfp*mesh->Nfaces*e + mesh->Nfp*f+n);
	dfloat sJ   = mesh->sgeo[sid + SJID];
	dfloat invJ = mesh->sgeo[sid + IJID];
	
	// A = 0.5*h*L
	// => J*2 = 0.5*h*sJ*2
	// => h = 2*J/sJ
	
	dfloat hest = 2./(sJ*invJ);
	
	hmin = mymin(hmin, hest);
	hmax = mymax(hmax, hest);
      }
    }
  }
    
  dfloat cfl = .8; // depends on the stability region size (was .4)

  // dt ~ cfl (h/(N+1)^2)/(Lambda^2*fastest wave speed)
  dfloat dt = cfl*hmin/((mesh->N+1.)*(mesh->N+1.)*sqrt(3.)*mesh->sqrtRT);
  
  printf("hmin = %g\n", hmin);
  printf("hmax = %g\n", hmax);
  printf("cfl = %g\n", cfl);
  printf("dt = %g\n", dt);
  printf("max wave speed = %g\n", sqrt(3.)*mesh->sqrtRT);
  
  // MPI_Allreduce to get global minimum dt
  MPI_Allreduce(&dt, &(mesh->dt), 1, MPI_DFLOAT, MPI_MIN, MPI_COMM_WORLD);

  //
  mesh->finalTime = 50;
  mesh->NtimeSteps = mesh->finalTime/mesh->dt;
  mesh->dt = mesh->finalTime/mesh->NtimeSteps;

  // errorStep
  mesh->errorStep = 10000;

  printf("dt = %g\n", mesh->dt);

  // OCCA build stuff
  char deviceConfig[BUFSIZ];
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  // use rank to choose DEVICE
  sprintf(deviceConfig, "mode = CUDA, deviceID = %d", (rank+1)%3);
  //  sprintf(deviceConfig, "mode = OpenCL, deviceID = 0, platformID = 1");
  //  sprintf(deviceConfig, "mode = OpenMP, deviceID = %d", 1);
  //sprintf(deviceConfig, "mode = Serial");	  

  occa::kernelInfo kernelInfo;

  // fixed to set up quad info on device too
  boltzmannOccaSetupQuad3D(mesh, deviceConfig,  kernelInfo);
  
  // quad stuff
  
  kernelInfo.addDefine("p_Nq", mesh->Nq);
  
  printf("mesh->Nq = %d\n", mesh->Nq);
  mesh->o_D  = mesh->device.malloc(mesh->Nq*mesh->Nq*sizeof(dfloat), mesh->D);

  mesh->o_vgeo =
    mesh->device.malloc(mesh->Nelements*mesh->Np*mesh->Nvgeo*sizeof(dfloat),
			mesh->vgeo);
  
  mesh->o_sgeo =
    mesh->device.malloc(mesh->Nelements*mesh->Nfp*mesh->Nfaces*mesh->Nsgeo*sizeof(dfloat),
			mesh->sgeo);

  // specialization for Boltzmann

  kernelInfo.addDefine("p_maxNodesVolume", mymax(mesh->cubNp,mesh->Np));
  
  int maxNodes = mymax(mesh->Np, (mesh->Nfp*mesh->Nfaces));
  kernelInfo.addDefine("p_maxNodes", maxNodes);

  int NblockV = 512/mesh->Np; // works for CUDA
  kernelInfo.addDefine("p_NblockV", NblockV);

  int NblockS = 512/maxNodes; // works for CUDA
  kernelInfo.addDefine("p_NblockS", NblockS);

  // physics 
  kernelInfo.addDefine("p_Lambda2", 0.5f);
  kernelInfo.addDefine("p_sqrtRT", mesh->sqrtRT);
  kernelInfo.addDefine("p_sqrt2", (float)sqrt(2.));
  kernelInfo.addDefine("p_invsqrt2", (float)sqrt(1./2.));
  kernelInfo.addDefine("p_isq12", (float)sqrt(1./12.));
  kernelInfo.addDefine("p_isq6", (float)sqrt(1./6.));
  kernelInfo.addDefine("p_tauInv", mesh->tauInv);


  kernelInfo.addDefine("p_q1bar", q1bar);
  kernelInfo.addDefine("p_q2bar", q2bar);
  kernelInfo.addDefine("p_q3bar", q3bar);
  kernelInfo.addDefine("p_q4bar", q4bar);
  kernelInfo.addDefine("p_q5bar", q5bar);
  kernelInfo.addDefine("p_q6bar", q6bar);
  kernelInfo.addDefine("p_q7bar", q7bar);
  kernelInfo.addDefine("p_q8bar", q8bar);
  kernelInfo.addDefine("p_q9bar", q9bar);
  kernelInfo.addDefine("p_q10bar", q10bar);

  kernelInfo.addDefine("p_alpha0", (float).01f);

  mesh->volumeKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/boltzmannVolumeQuad3D.okl",
				       "boltzmannVolumeQuad3D",
				       kernelInfo);
  printf("starting surface\n");
  mesh->surfaceKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/boltzmannSurfaceQuad3D.okl",
				       "boltzmannSurfaceQuad3D",
				       kernelInfo);
  printf("ending surface\n");

  mesh->updateKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/boltzmannUpdateQuad3D.okl",
				       "boltzmannUpdateQuad3D",
				       kernelInfo);

  mesh->haloExtractKernel =
    mesh->device.buildKernelFromSource(DHOLMES "/okl/meshHaloExtract2D.okl",
				       "meshHaloExtract2D",
				       kernelInfo);
  
}
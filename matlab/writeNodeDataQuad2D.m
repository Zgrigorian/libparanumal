
function writeNodeDataQuad2D(N)

r1d = JacobiGL(0,0,N);
V1d = Vandermonde1D(N, r1d);
D1d = Dmatrix1D(N, r1d, V1d);
M1d = inv(V1d')/V1d;
w1d = sum(M1d);
cnt = 1;
for i=1:N+1
  for j=1:N+1
    r(cnt) = r1d(j); %% r runs fastest
    s(cnt) = r1d(i);
    cnt = cnt+1;
  end
end
Np = (N+1)*(N+1);
r = reshape(r, Np,1);
s = reshape(s, Np,1);

Nfp = N+1;
Nfaces = 4;

% find all the nodes that lie on each edge
NODETOL = 1e-8;
faceNodes1   = find( abs(s+1) < NODETOL)'; 
faceNodes2   = find( abs(r-1) < NODETOL)';
faceNodes3   = find( abs(s-1) < NODETOL)';
faceNodes4   = find( abs(r+1) < NODETOL)';
faceNodes  = [faceNodes1;faceNodes2;faceNodes3;faceNodes4]';

V = VandermondeQuad2D(N, r, s);
[Dr,Ds] = DmatricesQuad2D(N, r, s, V);
LIFT = LiftQuad2D(N, faceNodes, r, s);

fname = sprintf('quadrilateralN%02d.dat', N);

fid = fopen(fname, 'w');

fprintf(fid, '%% degree N\n');
fprintf(fid, '%d\n', N);
fprintf(fid, '%% number of nodes\n');
fprintf(fid, '%d\n', Np);
fprintf(fid, '%% node coordinates\n');
for n=1:Np
  fprintf(fid, '%17.15E %17.15E\n', r(n), s(n));
end

fprintf(fid, '%% r collocation differentation matrix\n');
for n=1:Np
  for m=1:Np
    fprintf(fid, '%17.15E ', Dr(n,m));
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% s collocation differentation matrix\n');
for n=1:Np
  for m=1:Np
    fprintf(fid, '%17.15E ', Ds(n,m));
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% faceNodes\n');
for f=1:Nfaces
  for m=1:Nfp
    fprintf(fid, '%d ', faceNodes(m,f)-1); %% adjust for 0-indexing
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% LIFT matrix\n');
for n=1:Np
  for m=1:Nfp*Nfaces
    fprintf(fid, '%17.15E ', LIFT(n,m));
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% D (1D) matrix\n');
for n=1:N+1
  for m=1:N+1
    fprintf(fid, '%17.15E ', D1d(n,m));
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% r (1D) gll nodes\n');
for n=1:N+1
fprintf(fid, '%17.15E \n', r1d(n));
end

fprintf(fid, '%% w (1D) gll node weights\n');
for n=1:N+1
fprintf(fid, '%17.15E \n', w1d(n));
end


%% compute equispaced nodes on equilateral triangle
[plotR,plotS] = meshgrid(linspace(-1,1,N+4));
plotR = plotR(:); plotS = plotS(:);

%% count plot nodes
plotNp = length(plotR);

%% triangulate equilateral element nodes
plotEToV = delaunay(plotR,plotS)-1; 

%% count triangles in plot node triangulation
plotNelements = size(plotEToV,1); 

%% create interpolation matrix from warp & blend to plot nodes
plotInterp = VandermondeQuad2D(N, plotR,plotS)/V; 

%% output plot nodes
fprintf(fid, '%% number of plot nodes\n');
fprintf(fid, '%d\n', plotNp);
fprintf(fid, '%% plot node coordinates\n');
for n=1:plotNp
  fprintf(fid, '%17.15E %17.15E\n', plotR(n), plotS(n));
end

%% output plot interpolation matrix
fprintf(fid, '%% plot node interpolation matrix\n');
for n=1:plotNp
  for m=1:Np
    fprintf(fid, '%17.15E ', plotInterp(n,m));
  end
  fprintf(fid, '\n');
end

%% output plot triangulation
fprintf(fid, '%% number of plot elements\n');
fprintf(fid, '%d\n', plotNelements);

fprintf(fid, '%% number of vertices per plot elements\n');
fprintf(fid, '%d\n', size(plotEToV,2));

fprintf(fid, '%% triangulation of plot nodes\n');
for n=1:plotNelements
  fprintf(fid, '%d %d %d\n' ,...
 	plotEToV(n,1),plotEToV(n,2),plotEToV(n,3));
end

%% volume cubature
[z,w] = JacobiGQ(0,0,ceil(3*N/2));
[cubr,cubs] = meshgrid(z);
cubw = w*transpose(w);
cubr = cubr(:);
cubs = cubs(:);
cubw = cubw(:);

cInterp = VandermondeQuad2D(N, cubr, cubs)/V;
Ncub = length(cubr);

fprintf(fid, '%% number of volume cubature nodes\n');
fprintf(fid, '%d\n', length(cubr));
fprintf(fid, '%% cubature interpolation matrix\n');
for n=1:Ncub
  for m=1:Np
    fprintf(fid, '%17.15E ', cInterp(n,m));
  end
  fprintf(fid, '\n');
end

cV = VandermondeQuad2D(N, cubr, cubs);
cV'*diag(cubw)*cV;

[cVr,cVs] = GradVandermondeQuad2D(N, cubr, cubs);
cubDrT = V*transpose(cVr)*diag(cubw);
cubDsT = V*transpose(cVs)*diag(cubw);
cubProject = V*cV'*diag(cubw); %% relies on (transpose(cV)*diag(cubw)*cV being the identity)

fprintf(fid, '%% cubature r weak differentiation matrix\n');
for n=1:Np
  for m=1:Ncub
    fprintf(fid, '%17.15E ', cubDrT(n,m));
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% cubature s weak differentiation matrix\n');
for n=1:Np
  for m=1:Ncub
    fprintf(fid, '%17.15E ', cubDsT(n,m));
  end
  fprintf(fid, '\n');
end

fprintf(fid, '%% cubature projection matrix\n');
for n=1:Np
  for m=1:Ncub
    fprintf(fid, '%17.15E ', cubProject(n,m));
  end
  fprintf(fid, '\n');
end
cubProject
testIdentity = cubProject*cInterp


%% surface cubature
[z,w] = JacobiGQ(0,0,ceil(3*N/2));
%z = JacobiGL(0,0,N);
%zV = Vandermonde1D(N,z);
%w = sum(inv(zV*transpose(zV)));

Nfi = length(z);

ir = [z,ones(Nfi,1),-z,-ones(Nfi,1)];
is = [-ones(Nfi,1), z, ones(Nfi,1), -z];
iw = [w,w,w,w];

sV = VandermondeQuad2D(N, ir(:), is(:));
	    sInterp = sV/V;
	    
	    iInterp = [sInterp(1:Nfi,faceNodes(:,1));
	    sInterp(Nfi+1:2*Nfi,faceNodes(:,2));
	    sInterp(2*Nfi+1:3*Nfi,faceNodes(:,3));
	    sInterp(3*Nfi+1:4*Nfi,faceNodes(:,4))];

fprintf(fid, '%% number of surface integration nodes per face\n');
fprintf(fid, '%d\n', length(z));
fprintf(fid, '%% surface integration interpolation matrix\n');
for n=1:Nfi*Nfaces
  for m=1:Nfp
    fprintf(fid, '%17.15E ', iInterp(n,m));
  end
  fprintf(fid, '\n');
end

bInterp = [];
bInterp(1:Nfi,1:Nfp) = iInterp(1:Nfi,:);
bInterp(Nfi+1:2*Nfi,Nfp+1:2*Nfp) = iInterp(Nfi+1:2*Nfi,:);
bInterp(2*Nfi+1:3*Nfi,2*Nfp+1:3*Nfp) = iInterp(2*Nfi+1:3*Nfi,:);
bInterp(3*Nfi+1:4*Nfi,3*Nfp+1:4*Nfp) = iInterp(3*Nfi+1:4*Nfi,:);
%% integration node lift matrix
iLIFT = V*V'*sInterp'*diag(iw(:));
size(iLIFT)
size(iInterp)
altLiftError = max(max(abs(iLIFT*bInterp-LIFT)))

fprintf(fid, '%% surface integration lift matrix\n');
for n=1:Np
  for m=1:Nfi*Nfaces
    fprintf(fid, '%17.15E ', iLIFT(n,m));
  end
  fprintf(fid, '\n');
end

cubDrT*ones(Ncub,1) 
nr = [zeros(Nfi,1);ones(Nfi,1);zeros(Nfi,1);-ones(Nfi,1)];
ns = [-ones(Nfi,1);zeros(Nfi,1);ones(Nfi,1);zeros(Nfi,1)];
sJ = [ones(Nfi,1);ones(Nfi,1);ones(Nfi,1);ones(Nfi,1)];
cubDrT*ones(Ncub,1) - iLIFT*(nr.*sJ)
cubDsT*ones(Ncub,1) - iLIFT*(ns.*sJ)






fclose(fid);
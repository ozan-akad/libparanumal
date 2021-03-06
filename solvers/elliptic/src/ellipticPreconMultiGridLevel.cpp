/*

The MIT License (MIT)

Copyright (c) 2017 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "elliptic.hpp"
#include "ellipticPrecon.hpp"

void MGLevel::Operator(occa::memory &o_X, occa::memory &o_Ax) {
  elliptic.Operator(o_X,o_Ax);
}

void MGLevel::residual(occa::memory &o_RHS, occa::memory &o_X, occa::memory &o_RES) {
  elliptic.Operator(o_X,o_RES);

  // subtract res = rhs - A*x
  linAlg.axpy(elliptic.Ndofs, 1.f, o_RHS, -1.f, o_RES);
}

void MGLevel::coarsen(occa::memory &o_X, occa::memory &o_Rx) {

  if (elliptic.disc_c0) {
    //scratch spaces
    occa::memory &o_wx = o_smootherResidual;
    occa::memory &o_RxL = o_transferScratch;

    //pre-weight
    linAlg.amxpy(elliptic.Ndofs, 1.0, elliptic.o_weightG, o_X, 0.0, o_wx);

    elliptic.ogsMasked->GatheredHaloExchangeStart(o_wx, 1, ogs_dfloat);

    if(mesh.NlocalGatherElements)
      partialCoarsenKernel(mesh.NlocalGatherElements,
                           mesh.o_localGatherElementList,
                           elliptic.ogsMasked->o_GlobalToLocal,
                           o_P, o_wx, o_RxL);

    elliptic.ogsMasked->GatheredHaloExchangeFinish(o_wx, 1, ogs_dfloat);

    if(mesh.NglobalGatherElements)
      partialCoarsenKernel(mesh.NglobalGatherElements,
                           mesh.o_globalGatherElementList,
                           elliptic.ogsMasked->o_GlobalToLocal,
                           o_P, o_wx, o_RxL);

    ogsMaskedC->Gather(o_Rx, o_RxL, ogs_dfloat, ogs_add, ogs_trans);

  } else {
    coarsenKernel(mesh.Nelements, o_P, o_X, o_Rx);
  }
}

void MGLevel::prolongate(occa::memory &o_X, occa::memory &o_Px) {
  if (elliptic.disc_c0) {
    //scratch spaces
    occa::memory &o_PxG = o_smootherResidual;
    occa::memory &o_PxL = o_transferScratch;

    ogsMaskedC->GatheredHaloExchangeStart(o_X, 1, ogs_dfloat);

    if(mesh.NlocalGatherElements)
      partialProlongateKernel(meshC->NlocalGatherElements,
                              meshC->o_localGatherElementList,
                              ogsMaskedC->o_GlobalToLocal,
                              o_P, o_X, o_PxL);

    ogsMaskedC->GatheredHaloExchangeFinish(o_X, 1, ogs_dfloat);

    if(mesh.NglobalGatherElements)
      partialProlongateKernel(meshC->NglobalGatherElements,
                              meshC->o_globalGatherElementList,
                              ogsMaskedC->o_GlobalToLocal,
                              o_P, o_X, o_PxL);

    //ogs_notrans -> no summation at repeated nodes, just one value
    elliptic.ogsMasked->Gather(o_PxG, o_PxL, ogs_dfloat, ogs_add, ogs_notrans);

    linAlg.axpy(elliptic.Ndofs, 1.f, o_PxG, 1.f, o_Px);

  } else {
    prolongateKernel(mesh.Nelements, o_P, o_X, o_Px);
  }
}

void MGLevel::smooth(occa::memory &o_RHS, occa::memory &o_X, bool x_is_zero) {
  if (stype==JACOBI) {
    smoothJacobi(o_RHS, o_X, x_is_zero);
  } else if (stype==CHEBYSHEV) {
    smoothChebyshev(o_RHS, o_X, x_is_zero);
  }
}

void MGLevel::smoothJacobi(occa::memory &o_r, occa::memory &o_X, bool xIsZero) {

  occa::memory &o_RES = o_smootherResidual;

  if (xIsZero) {
    linAlg.amxpy(elliptic.Ndofs, 1.0, o_invDiagA, o_r, 0.0, o_X);
    return;
  }

  //res = r-Ax
  Operator(o_X,o_RES);
  linAlg.axpy(elliptic.Ndofs, 1.f, o_r, -1.f, o_RES);

  //smooth the fine problem x = x + S(r-Ax)
  linAlg.amxpy(elliptic.Ndofs, 1.0, o_invDiagA, o_RES, 1.0, o_X);
}

void MGLevel::smoothChebyshev (occa::memory &o_r, occa::memory &o_X, bool xIsZero) {

  const dfloat theta = 0.5*(lambda1+lambda0);
  const dfloat delta = 0.5*(lambda1-lambda0);
  const dfloat invTheta = 1.0/theta;
  const dfloat sigma = theta/delta;
  dfloat rho_n = 1./sigma;
  dfloat rho_np1;

  occa::memory &o_RES = o_smootherResidual;
  occa::memory &o_Ad  = o_smootherResidual2;
  occa::memory &o_d   = o_smootherUpdate;

  if(xIsZero){ //skip the Ax if x is zero
    //res = S*r
    linAlg.amxpy(elliptic.Ndofs, 1.0, o_invDiagA, o_r, 0.f, o_RES);

    //d = invTheta*res
    linAlg.axpy(elliptic.Ndofs, invTheta, o_RES, 0.f, o_d);
  } else {
    //res = S*(r-Ax)
    Operator(o_X,o_RES);
    linAlg.axpy(elliptic.Ndofs, 1.f, o_r, -1.f, o_RES);
    linAlg.amx(elliptic.Ndofs, 1.f, o_invDiagA, o_RES);

    //d = invTheta*res
    linAlg.axpy(elliptic.Ndofs, invTheta, o_RES, 0.f, o_d);
  }

  for (int k=0;k<ChebyshevIterations;k++) {
    //x_k+1 = x_k + d_k
    if (xIsZero&&(k==0))
      linAlg.axpy(elliptic.Ndofs, 1.f, o_d, 0.f, o_X);
    else
      linAlg.axpy(elliptic.Ndofs, 1.f, o_d, 1.f, o_X);

    //r_k+1 = r_k - SAd_k
    Operator(o_d,o_Ad);
    linAlg.amxpy(elliptic.Ndofs, -1.f, o_invDiagA, o_Ad, 1.f, o_RES);

    rho_np1 = 1.0/(2.*sigma-rho_n);
    dfloat rhoDivDelta = 2.0*rho_np1/delta;

    //d_k+1 = rho_k+1*rho_k*d_k  + 2*rho_k+1*r_k+1/delta
    linAlg.axpy(elliptic.Ndofs, rhoDivDelta, o_RES, rho_np1*rho_n, o_d);

    rho_n = rho_np1;
  }
  //x_k+1 = x_k + d_k
  linAlg.axpy(elliptic.Ndofs, 1.f, o_d, 1.0, o_X);
}


/******************************************
*
* MG Level Setup
*
*******************************************/

size_t  MGLevel::smootherResidualBytes=0;
size_t  MGLevel::scratchBytes=0;
dfloat* MGLevel::smootherResidual=nullptr;
occa::memory MGLevel::o_smootherResidual;
occa::memory MGLevel::o_smootherResidual2;
occa::memory MGLevel::o_smootherUpdate;
occa::memory MGLevel::o_transferScratch;

//build a level and connect it to the next one
MGLevel::MGLevel(elliptic_t& _elliptic,
                 dlong _Nrows, dlong _Ncols,
                 int Nc, int NpCoarse):
  multigridLevel(_Nrows, _Ncols,
                 _elliptic.platform, _elliptic.settings),
  elliptic(_elliptic),
  mesh(_elliptic.mesh),
  linAlg(_elliptic.linAlg) {

  SetupSmoother();
  AllocateStorage();

  if (mesh.elementType==QUADRILATERALS || mesh.elementType==HEXAHEDRA) {
    P = (dfloat *) calloc((mesh.N+1)*(Nc+1),sizeof(dfloat));
    mesh.DegreeRaiseMatrix1D(Nc, mesh.N, P);
    o_P = elliptic.platform.malloc((mesh.N+1)*(Nc+1)*sizeof(dfloat), P);
  } else if (mesh.elementType==TRIANGLES) {
    P = (dfloat *) calloc(mesh.Np*NpCoarse,sizeof(dfloat));
    mesh.DegreeRaiseMatrixTri2D(Nc, mesh.N, P);
    o_P = elliptic.platform.malloc(mesh.Np*NpCoarse*sizeof(dfloat), P);
  } else {
    P = (dfloat *) calloc(mesh.Np*NpCoarse,sizeof(dfloat));
    mesh.DegreeRaiseMatrixTet3D(Nc, mesh.N, P);
    o_P = elliptic.platform.malloc(mesh.Np*NpCoarse*sizeof(dfloat), P);
  }

  //build kernels
  occa::properties kernelInfo = elliptic.platform.props;

  // set kernel name suffix
  char *suffix;
  if(mesh.elementType==TRIANGLES)
    suffix = strdup("Tri2D");
  else if(mesh.elementType==QUADRILATERALS)
    suffix = strdup("Quad2D");
  else if(mesh.elementType==TETRAHEDRA)
    suffix = strdup("Tet3D");
  else if(mesh.elementType==HEXAHEDRA)
    suffix = strdup("Hex3D");

  char fileName[BUFSIZ], kernelName[BUFSIZ];

  kernelInfo["defines/" "p_NqFine"]= mesh.N+1;
  kernelInfo["defines/" "p_NqCoarse"]= Nc+1;

  kernelInfo["defines/" "p_NpFine"]= mesh.Np;
  kernelInfo["defines/" "p_NpCoarse"]= NpCoarse;

  int blockMax = 256;
  if (elliptic.platform.device.mode() == "CUDA") blockMax = 512;

  int NblockVFine = mymax(1,blockMax/mesh.Np);
  int NblockVCoarse = mymax(1,blockMax/NpCoarse);
  kernelInfo["defines/" "p_NblockVFine"]= NblockVFine;
  kernelInfo["defines/" "p_NblockVCoarse"]= NblockVCoarse;

  if (settings.compareSetting("DISCRETIZATION", "CONTINUOUS")) {
    sprintf(fileName, DELLIPTIC "/okl/ellipticPreconCoarsen%s.okl", suffix);
    sprintf(kernelName, "ellipticPartialPreconCoarsen%s", suffix);
    partialCoarsenKernel = elliptic.platform.buildKernel(fileName, kernelName, kernelInfo);

    sprintf(fileName, DELLIPTIC "/okl/ellipticPreconProlongate%s.okl", suffix);
    sprintf(kernelName, "ellipticPartialPreconProlongate%s", suffix);
    partialProlongateKernel = elliptic.platform.buildKernel(fileName, kernelName, kernelInfo);
  } else { //IPDG
    sprintf(fileName, DELLIPTIC "/okl/ellipticPreconCoarsen%s.okl", suffix);
    sprintf(kernelName, "ellipticPreconCoarsen%s", suffix);
    coarsenKernel = elliptic.platform.buildKernel(fileName, kernelName, kernelInfo);

    sprintf(fileName, DELLIPTIC "/okl/ellipticPreconProlongate%s.okl", suffix);
    sprintf(kernelName, "ellipticPreconProlongate%s", suffix);
    prolongateKernel = elliptic.platform.buildKernel(fileName, kernelName, kernelInfo);
  }
}

void MGLevel::AllocateStorage() {
  // extra storage for smoothing op
  size_t Nbytes = Ncols*sizeof(dfloat);
  if (smootherResidualBytes < Nbytes) {
    if (o_smootherResidual.size()) {
      free(smootherResidual);
      o_smootherResidual.free();
      o_smootherResidual2.free();
      o_smootherUpdate.free();
    }

    smootherResidual = (dfloat *) calloc(Ncols,sizeof(dfloat));
    o_smootherResidual = elliptic.platform.malloc(Nbytes,smootherResidual);
    o_smootherResidual2 = elliptic.platform.malloc(Nbytes,smootherResidual);
    o_smootherUpdate = elliptic.platform.malloc(Nbytes,smootherResidual);
    smootherResidualBytes = Nbytes;
  }

  Nbytes = mesh.Nelements*mesh.Np*sizeof(dfloat);
  if (scratchBytes < Nbytes) {
    if (o_transferScratch.size()) {
      o_transferScratch.free();
    }
    dfloat *dummy = (dfloat *) calloc(mesh.Nelements*mesh.Np,sizeof(dfloat));
    o_transferScratch = elliptic.platform.malloc(Nbytes, dummy);
    free(dummy);
    scratchBytes = Nbytes;
  }
}

void MGLevel::Report() {

  hlong hNrows = (hlong) Nrows;

  dlong minNrows=0, maxNrows=0;
  hlong totalNrows=0;
  dfloat avgNrows;

  MPI_Allreduce(&Nrows, &maxNrows, 1, MPI_DLONG, MPI_MAX, mesh.comm);
  MPI_Allreduce(&hNrows, &totalNrows, 1, MPI_HLONG, MPI_SUM, mesh.comm);
  avgNrows = (dfloat) totalNrows/mesh.size;

  if (Nrows==0) Nrows=maxNrows; //set this so it's ignored for the global min
  MPI_Allreduce(&Nrows, &minNrows, 1, MPI_DLONG, MPI_MIN, mesh.comm);

  char smootherString[BUFSIZ];
  if (stype==JACOBI)
    strcpy(smootherString, "Damped Jacobi   ");
  else if (stype==CHEBYSHEV)
    strcpy(smootherString, "Chebyshev       ");

  //This setup can be called by many subcommunicators, so only
  // print on the global root.
  if (mesh.rank==0){
    printf(      "|    pMG     |    %10lld  |    %10d  |   Matrix-free   |   %s|\n", (long long int)totalNrows, minNrows, smootherString);
    printf("      |            |                |    %10d  |     Degree %2d   |                   |\n", maxNrows, mesh.N);
    printf("      |            |                |    %10d  |                 |                   |\n", (int) avgNrows);
  }
}

MGLevel::~MGLevel() {
  coarsenKernel.free();
  partialCoarsenKernel.free();
  prolongateKernel.free();
  partialProlongateKernel.free();
}

void MGLevel::SetupSmoother() {

  //set up the fine problem smoothing
  dfloat *diagA    = (dfloat*) calloc(Nrows, sizeof(dfloat));
  dfloat *invDiagA = (dfloat*) calloc(Nrows, sizeof(dfloat));
  elliptic.BuildOperatorDiagonal(diagA);

  for (dlong n=0;n<Nrows;n++)
    invDiagA[n] = 1.0/diagA[n];

  o_invDiagA = elliptic.platform.malloc(Nrows*sizeof(dfloat), invDiagA);

  if (elliptic.settings.compareSetting("MULTIGRID SMOOTHER","CHEBYSHEV")) {
    stype = CHEBYSHEV;

    ChebyshevIterations = 2; //default to degree 2
    elliptic.settings.getSetting("MULTIGRID CHEBYSHEV DEGREE", ChebyshevIterations);

    //estimate the max eigenvalue of S*A
    dfloat rho = maxEigSmoothAx();

    lambda1 = rho;
    lambda0 = rho/10.;
  } else {
    stype = JACOBI;

    //estimate the max eigenvalue of S*A
    dfloat rho = maxEigSmoothAx();

    //set the stabilty weight (jacobi-type interation)
    lambda0 = (4./3.)/rho;

    for (dlong n=0;n<Nrows;n++)
      invDiagA[n] *= lambda0;

    //update diagonal with weight
    o_invDiagA.copyFrom(invDiagA);
  }
  free(diagA);
  free(invDiagA);
}


//------------------------------------------------------------------------
//
//  Estimate max Eigenvalue of diagA^{-1}*A
//
//------------------------------------------------------------------------

dfloat MGLevel::maxEigSmoothAx(){

  const dlong N = Nrows;
  const dlong M = Ncols;

  int k = 10;

  hlong Nlocal = (hlong) Nrows;
  hlong Ntotal = 0;
  MPI_Allreduce(&Nlocal, &Ntotal, 1, MPI_HLONG, MPI_SUM, mesh.comm);
  if(k > Ntotal) k = static_cast<int>(Ntotal);

  // do an arnoldi

  // allocate memory for Hessenberg matrix
  double *H = (double *) calloc(k*k,sizeof(double));

  // allocate memory for basis
  dfloat *Vx = (dfloat*) calloc(M, sizeof(dfloat));
  //  occa::memory *o_V = (occa::memory *) calloc(k+1, sizeof(occa::memory));
  occa::memory *o_V = new occa::memory[k+1];

  occa::memory o_Vx  = elliptic.platform.malloc(M*sizeof(dfloat),Vx);
  occa::memory o_AVx = elliptic.platform.malloc(M*sizeof(dfloat),Vx);

  for(int i=0; i<=k; i++)
    o_V[i] = elliptic.platform.malloc(M*sizeof(dfloat),Vx);

  // generate a random vector for initial basis vector
  for (dlong i=0;i<N;i++) Vx[i] = (dfloat) drand48();

  o_Vx.copyFrom(Vx); //copy to device

  dfloat norm_vo =  linAlg.norm2(N, o_Vx, mesh.comm);

  linAlg.axpy(N, 1./norm_vo, o_Vx, 0.f, o_V[0]);

  for(int j=0; j<k; j++){
    // v[j+1] = invD*(A*v[j])
    Operator(o_V[j],o_AVx);
    linAlg.amxpy(N, 1.0, o_invDiagA, o_AVx, 0.0, o_V[j+1]);

    // modified Gram-Schmidth
    for(int i=0; i<=j; i++){
      // H(i,j) = v[i]'*A*v[j]
      dfloat hij =  linAlg.innerProd(N, o_V[i], o_V[j+1], mesh.comm);

      // v[j+1] = v[j+1] - hij*v[i]
      linAlg.axpy(N, -hij, o_V[i], 1.f, o_V[j+1]);

      H[i + j*k] = (double) hij;
    }

    if(j+1 < k){
      // v[j+1] = v[j+1]/||v[j+1]||
      dfloat norm_vj =  linAlg.norm2(N, o_V[j+1], mesh.comm);
      linAlg.scale(N, 1.0/norm_vj, o_V[j+1]);

      H[j+1+ j*k] = (double) norm_vj;
    }
  }

  double *WR = (double *) malloc(k*sizeof(double));
  double *WI = (double *) malloc(k*sizeof(double));

  matrixEigenValues(k, H, WR, WI);

  double rho = 0.;

  for(int i=0; i<k; i++){
    double rho_i  = sqrt(WR[i]*WR[i] + WI[i]*WI[i]);

    if(rho < rho_i) {
      rho = rho_i;
    }
  }

  // free memory
  free(H);
  free(WR);
  free(WI);

  free(Vx);
  o_Vx.free();
  o_AVx.free();
  for(int i=0; i<=k; i++) o_V[i].free();
  delete[] o_V;

  // if((mesh.rank==0)) printf("weight = %g \n", rho);

  return rho;
}

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

#include "linearSolver.hpp"

#define PCG_BLOCKSIZE 512

pcg::pcg(dlong _N, dlong _Nhalo,
         platform_t& _platform, settings_t& _settings, MPI_Comm _comm):
  linearSolver_t(_N, _Nhalo, _platform, _settings, _comm) {

  dlong Ntotal = N + Nhalo;

  flexible = settings.compareSetting("LINEAR SOLVER", "FPCG");

  /*aux variables */
  dfloat *dummy = (dfloat *) calloc(Ntotal,sizeof(dfloat)); //need this to avoid uninitialized memory warnings
  o_p  = platform.malloc(Ntotal*sizeof(dfloat),dummy);
  o_z  = platform.malloc(Ntotal*sizeof(dfloat),dummy);
  o_Ax = platform.malloc(Ntotal*sizeof(dfloat),dummy);
  o_Ap = platform.malloc(Ntotal*sizeof(dfloat),dummy);
  free(dummy);

  //pinned tmp buffer for reductions
  tmprdotr = (dfloat*) platform.hostMalloc(PCG_BLOCKSIZE*sizeof(dfloat),
                                          NULL, h_tmprdotr);
  o_tmprdotr = platform.malloc(PCG_BLOCKSIZE*sizeof(dfloat));

  /* build kernels */
  occa::properties kernelInfo = platform.props; //copy base properties

  //add defines
  kernelInfo["defines/" "p_blockSize"] = (int)PCG_BLOCKSIZE;

  // combined PCG update and r.r kernel
  updatePCGKernel = platform.buildKernel(LINEARSOLVER_DIR "/okl/linearSolverUpdatePCG.okl",
                                "updatePCG", kernelInfo);
}

int pcg::Solve(solver_t& solver, precon_t& precon,
               occa::memory &o_x, occa::memory &o_r,
               const dfloat tol, const int MAXIT, const int verbose) {

  int rank;
  MPI_Comm_rank(comm, &rank);
  linAlg_t &linAlg = platform.linAlg;

  // register scalars
  dfloat rdotz1 = 0.0;
  dfloat rdotz2 = 0.0;
  dfloat alpha = 0.0, beta = 0.0, pAp = 0.0;
  dfloat rdotr = 0.0;
  dfloat TOL = 0.0;

  // Comput norm of RHS (for stopping tolerance).
  if (settings.compareSetting("LINEAR SOLVER STOPPING CRITERION", "ABS/REL-RHS-2NORM")) {
    dfloat normb = linAlg.norm2(N, o_r, comm);
    TOL = mymax(tol*tol*normb*normb, tol*tol);
  }

  // compute A*x
  solver.Operator(o_x, o_Ax);

  // subtract r = r - A*x
  linAlg.axpy(N, -1.f, o_Ax, 1.f, o_r);

  rdotr = linAlg.norm2(N, o_r, comm);
  rdotr = rdotr*rdotr;

  if (settings.compareSetting("LINEAR SOLVER STOPPING CRITERION", "ABS/REL-INITRESID")) {
    TOL = mymax(tol*tol*rdotr,tol*tol);
  }

  if (verbose&&(rank==0))
    printf("PCG: initial res norm %12.12f \n", sqrt(rdotr));

  int iter;
  for(iter=0;iter<MAXIT;++iter){

    // Exit if tolerance is reached, taking at least one step.
    if (((iter == 0) && (rdotr == 0.0)) ||
        ((iter > 0) && (rdotr <= TOL))) {
      break;
    }

    // z = Precon^{-1} r
    precon.Operator(o_r, o_z);

    // r.z
    rdotz2 = rdotz1;
    rdotz1 = linAlg.innerProd(N, o_r, o_z, comm);

    if(flexible){
      dfloat zdotAp = linAlg.innerProd(N, o_z, o_Ap, comm);
      beta = (iter==0) ? 0.0 : -alpha*zdotAp/rdotz2;
    } else {
      beta = (iter==0) ? 0.0 : rdotz1/rdotz2;
    }

    // p = z + beta*p
    linAlg.axpy(N, 1.f, o_z, beta, o_p);

    // A*p
    solver.Operator(o_p, o_Ap);

    // p.Ap
    pAp =  linAlg.innerProd(N, o_p, o_Ap, comm);

    alpha = rdotz1/pAp;

    //  x <= x + alpha*p
    //  r <= r - alpha*A*p
    //  dot(r,r)
    rdotr = UpdatePCG(alpha, o_x, o_r);

    if (verbose&&(rank==0)) {
      if(rdotr<0)
        printf("WARNING CG: rdotr = %17.15lf\n", rdotr);

      printf("CG: it %d, r norm %12.12le, alpha = %le \n", iter+1, sqrt(rdotr), alpha);
    }
  }

  return iter;
}

dfloat pcg::UpdatePCG(const dfloat alpha, occa::memory &o_x, occa::memory &o_r){

  // x <= x + alpha*p
  // r <= r - alpha*A*p
  // dot(r,r)
  int Nblocks = (N+PCG_BLOCKSIZE-1)/PCG_BLOCKSIZE;
  Nblocks = (Nblocks>PCG_BLOCKSIZE) ? PCG_BLOCKSIZE : Nblocks; //limit to PCG_BLOCKSIZE entries

  updatePCGKernel(N, Nblocks, o_p, o_Ap, alpha, o_x, o_r, o_tmprdotr);

  o_tmprdotr.copyTo(tmprdotr, Nblocks*sizeof(dfloat));

  dfloat rdotr1 = 0;
  for(int n=0;n<Nblocks;++n)
    rdotr1 += tmprdotr[n];

  dfloat globalrdotr1 = 0;
  MPI_Allreduce(&rdotr1, &globalrdotr1, 1, MPI_DFLOAT, MPI_SUM, comm);
  return globalrdotr1;
}

pcg::~pcg() {
  updatePCGKernel.free();
}

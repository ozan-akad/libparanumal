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


#include "ellipticTri2D.h"

void BuildLocalIpdgPatchAx(solver_t* solver, mesh2D* mesh, int basisNp, dfloat *basis, dfloat tau, dfloat lambda, int* BCType,
                        dfloat *MS, dlong eM, dfloat *A);
void BuildLocalBRdgPatchAx(solver_t* solver, mesh2D* mesh, int basisNp, dfloat *basis, dfloat tau, dfloat lambda, int* BCType,
                        dfloat *MS, dlong eM, dfloat *A);

void BuildLocalContinuousPatchAx(solver_t* solver, mesh2D* mesh, dfloat lambda,
                                  dlong eM, dfloat *Srr, dfloat *Srs, 
                                  dfloat *Sss, dfloat *MM, dfloat *A);


void ellipticBuildJacobiTri2D(solver_t* solver, mesh2D* mesh, int basisNp, dfloat *basis,
                                   dfloat tau, dfloat lambda,
                                   int *BCType, dfloat **invDiagA,
                                   const char *options){

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  if(!basis) { // default to degree N Lagrange basis
    basisNp = mesh->Np;
    basis = (dfloat*) calloc(basisNp*basisNp, sizeof(dfloat));
    for(int n=0;n<basisNp;++n){
      basis[n+n*basisNp] = 1;
    }
  }

  // surface mass matrices MS = MM*LIFT
  dfloat *MS = (dfloat *) calloc(mesh->Nfaces*mesh->Nfp*mesh->Nfp,sizeof(dfloat));
  for (int f=0;f<mesh->Nfaces;f++) {
    for (int n=0;n<mesh->Nfp;n++) {
      int fn = mesh->faceNodes[f*mesh->Nfp+n];

      for (int m=0;m<mesh->Nfp;m++) {
        dfloat MSnm = 0;

        for (int i=0;i<mesh->Np;i++){
          MSnm += mesh->MM[fn+i*mesh->Np]*mesh->LIFT[i*mesh->Nfp*mesh->Nfaces+f*mesh->Nfp+m];
        }

        MS[m+n*mesh->Nfp + f*mesh->Nfp*mesh->Nfp]  = MSnm;
      }
    }
  }

  dfloat *Srr, *Srs, *Sss, *MM;
  if (strstr(options,"CONTINUOUS")) {
    Srr = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    Srs = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    Sss = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));
    MM = (dfloat *) calloc(mesh->Np*mesh->Np,sizeof(dfloat));

    if (strstr(options,"SPARSE")) {
      for (int k=0;k<mesh->SparseNnzPerRow;k++) {
        for (int n=0;n<mesh->Np;n++) {
          int m = mesh->sparseStackedNZ[n+k*mesh->Np]-1;
          if (m>-1) {
            Srr[m+n*mesh->Np] = mesh->sparseSrrT[n+k*mesh->Np];
            Srs[m+n*mesh->Np] = mesh->sparseSrsT[n+k*mesh->Np];
            Sss[m+n*mesh->Np] = mesh->sparseSssT[n+k*mesh->Np];  
          }
        }
      }
      for (int n=0;n<mesh->Np;n++) {
        for (int m=0;m<mesh->Np;m++) {
          MM[m+n*mesh->Np] = mesh->sparseMM[m+n*mesh->Np];
        }
      }
    } else {
      for (int n=0;n<mesh->Np;n++) {
        for (int m=0;m<mesh->Np;m++) {
          Srr[m+n*mesh->Np] = mesh->Srr[m+n*mesh->Np];
          Srs[m+n*mesh->Np] = mesh->Srs[m+n*mesh->Np] + mesh->Ssr[m+n*mesh->Np];
          Sss[m+n*mesh->Np] = mesh->Sss[m+n*mesh->Np];
          MM[m+n*mesh->Np] = mesh->MM[m+n*mesh->Np];
        }
      }
    }

  }

  dlong diagNnum = basisNp*mesh->Nelements;

  dfloat *diagA = (dfloat*) calloc(diagNnum, sizeof(dfloat));

  if(rank==0) printf("Building diagonal...");fflush(stdout);

  // loop over all elements
  #pragma omp parallel
  {
    //temp patch storage
    dfloat *patchA = (dfloat*) calloc(mesh->Np*mesh->Np, sizeof(dfloat));

    #pragma omp for 
    for(dlong eM=0;eM<mesh->Nelements;++eM){
      //build the patch A matrix for this element
      if (strstr(options,"IPDG")) {
        BuildLocalIpdgPatchAx(solver, mesh, basisNp, basis, tau, lambda, BCType, MS, eM, patchA);
      } else if (strstr(options,"BRDG")) {
        BuildLocalBRdgPatchAx(solver, mesh, basisNp, basis, tau, lambda, BCType, MS, eM, patchA);
      } else if (strstr(options,"CONTINUOUS")) {
        BuildLocalContinuousPatchAx(solver, mesh, lambda, eM, Srr, Srs, Sss, MM, patchA);
      }

      for(int n=0;n<mesh->Np;++n) {
        diagA[eM*mesh->Np + n] = patchA[n*mesh->Np+n]; //store the diagonal entry
      }
    }
    
    free(patchA);
  }

  if (strstr(options,"CONTINUOUS")) 
    gsParallelGatherScatter(mesh->hostGsh, diagA, dfloatString, "add"); 
    
  *invDiagA = (dfloat*) calloc(diagNnum, sizeof(dfloat));
  for (dlong n=0;n<mesh->Nelements*mesh->Np;n++) {
    (*invDiagA)[n] = 1/diagA[n];
  }

  if(rank==0) printf("done.\n");

  if (strstr(options,"CONTINUOUS")) {
    free(Srr);
    free(Srs);
    free(Sss);
    free(MM);
  }

  free(diagA);
  free(MS);
}

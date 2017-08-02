
#include "ellipticTri2D.h"

typedef struct{

  iint row;
  iint col;
  iint ownerRank;
  dfloat val;

} nonZero_t;


static iint addNonZero(nonZero_t *nonZeros, iint nnz, iint row, iint col, iint owner, dfloat val){

  dfloat tol = 1e-12;

  if(fabs(val)>tol){
    nonZeros[nnz].val = val;
    nonZeros[nnz].row = row;
    nonZeros[nnz].col = col;
    nonZeros[nnz].ownerRank = owner;
    ++nnz;
  }

  return nnz;
}

int parallelCompareRowColumn(const void *a, const void *b);

extern "C"
{
  void dgetrf_ (int *, int *, double *, int *, int *, int *);
  void dgetri_ (int *, double *, int *, int *, double *, int *, int *);
}

void matrixInverse(int N, dfloat *A){
  int lwork = N*N;
  iint info;

  // compute inverse mass matrix
  double *tmpInvA = (double*) calloc(N*N, sizeof(double));

  iint *ipiv = (iint*) calloc(N, sizeof(iint));
  double *work = (double*) calloc(lwork, sizeof(double));

  for(iint n=0;n<N*N;++n){
    tmpInvA[n] = A[n];
  }

  dgetrf_ (&N, &N, tmpInvA, &N, ipiv, &info);
  dgetri_ (&N, tmpInvA, &N, ipiv, work, &lwork, &info);

  if(info)
    printf("inv: dgetrf/dgetri reports info = %d when inverting the reference mass matrix\n", info);

  for(iint n=0;n<N*N;++n)
    A[n] = tmpInvA[n];

  free(work);
  free(ipiv);
  free(tmpInvA);
}


void ellipticBuildExactPatchesIpdgTri2D(mesh2D *mesh, iint basisNp, dfloat *basis,
                                   dfloat tau, dfloat lambda, iint *BCType, dfloat **patchesInvA, const char *options){

  iint size, rankM;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rankM);

  if(!basis) { // default to degree N Lagrange basis
    basisNp = mesh->Np;
    basis = (dfloat*) calloc(basisNp*basisNp, sizeof(dfloat));
    for(iint n=0;n<basisNp;++n){
      basis[n+n*basisNp] = 1;
    }
  }

  // number of degrees of freedom on this rank
  iint Nnum = basisNp*mesh->Nelements;

  // create a global numbering system
  iint *globalIds = (iint *) calloc((mesh->Nelements+mesh->totalHaloPairs)*basisNp,sizeof(iint));
  iint *globalOwners = (iint*) calloc(Nnum, sizeof(iint));

  // every degree of freedom has its own global id
  /* so find number of elements on each rank */
  iint *rankNelements = (iint*) calloc(size, sizeof(iint));
  iint *rankStarts = (iint*) calloc(size+1, sizeof(iint));
  MPI_Allgather(&(mesh->Nelements), 1, MPI_IINT,
                rankNelements, 1, MPI_IINT, MPI_COMM_WORLD);
  //find offsets
  for(iint r=0;r<size;++r){
    rankStarts[r+1] = rankStarts[r]+rankNelements[r];
  }
  //use the offsets to set a global id
  for (iint e =0;e<mesh->Nelements;e++) {
    for (int n=0;n<basisNp;n++) {
      globalIds[e*basisNp +n] = n + (e + rankStarts[rankM])*basisNp;
      globalOwners[e*basisNp +n] = rankM;
    }
  }

  /* do a halo exchange of global node numbers */
  if (mesh->totalHaloPairs) {
    iint *idSendBuffer = (iint *) calloc(basisNp*mesh->totalHaloPairs,sizeof(iint));
    meshHaloExchange(mesh, basisNp*sizeof(iint), globalIds, idSendBuffer, globalIds + mesh->Nelements*basisNp);
    free(idSendBuffer);
  }

  iint nnzLocalBound = basisNp*basisNp*(1+mesh->Nfaces)*mesh->Nelements;


  // surface mass matrices MS = MM*LIFT
  dfloat *MS = (dfloat *) calloc(mesh->Nfaces*mesh->Nfp*mesh->Nfp,sizeof(dfloat));
  for (iint f=0;f<mesh->Nfaces;f++) {
    for (iint n=0;n<mesh->Nfp;n++) {
      iint fn = mesh->faceNodes[f*mesh->Nfp+n];

      for (iint m=0;m<mesh->Nfp;m++) {
        dfloat MSnm = 0;

        for (iint i=0;i<mesh->Np;i++){
          MSnm += mesh->MM[fn+i*mesh->Np]*mesh->LIFT[i*mesh->Nfp*mesh->Nfaces+f*mesh->Nfp+m];
        }

        MS[m+n*mesh->Nfp + f*mesh->Nfp*mesh->Nfp]  = MSnm;
      }
    }
  }

  // reset non-zero counter
  int nnz = 0;

  nonZero_t *A = (nonZero_t*) calloc(nnzLocalBound, sizeof(nonZero_t));

  // TW: ONLY NON-MPI FOR THE MOMENT
  dfloat *blocksA = (dfloat *) calloc(mesh->Nelements*(mesh->Nfaces+1)*mesh->Np*mesh->Np,sizeof(dfloat));

  // loop over all elements
  for(iint eM=0;eM<mesh->Nelements;++eM){

    dfloat *SM = blocksA + eM*(mesh->Nfaces+1)*mesh->Np*mesh->Np;

    iint vbase = eM*mesh->Nvgeo;
    dfloat drdx = mesh->vgeo[vbase+RXID];
    dfloat drdy = mesh->vgeo[vbase+RYID];
    dfloat dsdx = mesh->vgeo[vbase+SXID];
    dfloat dsdy = mesh->vgeo[vbase+SYID];
    dfloat J = mesh->vgeo[vbase+JID];

    /* start with stiffness matrix  */
    for(iint n=0;n<mesh->Np;++n){
      for(iint m=0;m<mesh->Np;++m){
        SM[n*mesh->Np+m]  = J*lambda*mesh->MM[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*drdx*drdx*mesh->Srr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*drdx*dsdx*mesh->Srs[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdx*drdx*mesh->Ssr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdx*dsdx*mesh->Sss[n*mesh->Np+m];

        SM[n*mesh->Np+m] += J*drdy*drdy*mesh->Srr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*drdy*dsdy*mesh->Srs[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdy*drdy*mesh->Ssr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdy*dsdy*mesh->Sss[n*mesh->Np+m];
      }
    }

    for (iint fM=0;fM<mesh->Nfaces;fM++) {

      dfloat *SP = blocksA + (fM+1)*mesh->Np*mesh->Np + eM*mesh->Np*mesh->Np*(mesh->Nfaces+1);

      // load surface geofactors for this face
      iint sid = mesh->Nsgeo*(eM*mesh->Nfaces+fM);
      dfloat nx = mesh->sgeo[sid+NXID];
      dfloat ny = mesh->sgeo[sid+NYID];
      dfloat sJ = mesh->sgeo[sid+SJID];
      dfloat hinv = mesh->sgeo[sid+IHID];
      dfloat penalty = tau*hinv;

      iint eP = mesh->EToE[eM*mesh->Nfaces+fM];
      if (eP < 0) eP = eM;

      iint vbaseP = eP*mesh->Nvgeo;
      dfloat drdxP = mesh->vgeo[vbaseP+RXID];
      dfloat drdyP = mesh->vgeo[vbaseP+RYID];
      dfloat dsdxP = mesh->vgeo[vbaseP+SXID];
      dfloat dsdyP = mesh->vgeo[vbaseP+SYID];

      int bcD = 0, bcN =0;
      int bc = mesh->EToB[fM+mesh->Nfaces*eM]; //raw boundary flag
      iint bcType = 0;

      if(bc>0) bcType = BCType[bc];          //find its type (Dirichlet/Neumann)

      // this needs to be double checked (and the code where these are used)
      if(bcType==1){ // Dirichlet
        bcD = 1;
        bcN = 0;
      } else if(bcType==2){ // Neumann
        bcD = 0;
        bcN = 1;
      }

      // reset eP
      eP = mesh->EToE[eM*mesh->Nfaces+fM];

      // mass matrix for this face
      dfloat *MSf = MS+fM*mesh->Nfp*mesh->Nfp;

      // penalty term just involves face nodes
      for(iint n=0;n<mesh->Nfp;++n){
        for(iint m=0;m<mesh->Nfp;++m){
          iint nM = mesh->faceNodes[fM*mesh->Nfp+n];
          iint mM = mesh->faceNodes[fM*mesh->Nfp+m];

          // OP11 = OP11 + 0.5*( gtau*mmE )
          dfloat MSfnm = sJ*MSf[n*mesh->Nfp+m];
          SM[nM*mesh->Np+mM] += 0.5*(1.-bcN)*(1.+bcD)*penalty*MSfnm;

          // neighbor penalty term
          if(eP>=0){
            iint idM = eM*mesh->Nfp*mesh->Nfaces+fM*mesh->Nfp+m;
            iint mP  = mesh->vmapP[idM]%mesh->Np;

            // OP12(:,Fm2) = - 0.5*( gtau*mmE(:,Fm1) );
            SP[nM*mesh->Np+mP] += -0.5*penalty*MSfnm;
          }
        }
      }

      // now add differential surface terms
      for(iint n=0;n<mesh->Nfp;++n){
        for(iint m=0;m<mesh->Np;++m){
          iint nM = mesh->faceNodes[fM*mesh->Nfp+n];

          for(iint i=0;i<mesh->Nfp;++i){
            iint iM = mesh->faceNodes[fM*mesh->Nfp+i];
            iint iP = mesh->vmapP[i + fM*mesh->Nfp+eM*mesh->Nfp*mesh->Nfaces]%mesh->Np;

            dfloat MSfni = sJ*MSf[n*mesh->Nfp+i]; // surface Jacobian built in

            dfloat DxMim = drdx*mesh->Dr[iM*mesh->Np+m] + dsdx*mesh->Ds[iM*mesh->Np+m];
            dfloat DyMim = drdy*mesh->Dr[iM*mesh->Np+m] + dsdy*mesh->Ds[iM*mesh->Np+m];

            // OP11 = OP11 + 0.5*( - mmE*Dn1)
            SM[nM*mesh->Np+m] += -0.5*nx*(1+bcD)*(1-bcN)*MSfni*DxMim;
            SM[nM*mesh->Np+m] += -0.5*ny*(1+bcD)*(1-bcN)*MSfni*DyMim;

            if(eP>=0){

              dfloat DxPim = drdxP*mesh->Dr[iP*mesh->Np+m] + dsdxP*mesh->Ds[iP*mesh->Np+m];
              dfloat DyPim = drdyP*mesh->Dr[iP*mesh->Np+m] + dsdyP*mesh->Ds[iP*mesh->Np+m];

              //OP12(Fm1,:) = OP12(Fm1,:) - 0.5*(      mmE(Fm1,Fm1)*Dn2(Fm2,:) );
              SP[nM*mesh->Np+m] += -0.5*nx*MSfni*DxPim;
              SP[nM*mesh->Np+m] += -0.5*ny*MSfni*DyPim;
            }
          }
        }
      }

      for(iint n=0;n<mesh->Np;++n){
        for(iint m=0;m<mesh->Nfp;++m){
          iint mM = mesh->faceNodes[fM*mesh->Nfp+m];
          iint mP = mesh->vmapP[m + fM*mesh->Nfp+eM*mesh->Nfp*mesh->Nfaces]%mesh->Np;

          for(iint i=0;i<mesh->Nfp;++i){
            iint iM = mesh->faceNodes[fM*mesh->Nfp+i];

            dfloat MSfim = sJ*MSf[i*mesh->Nfp+m];

            dfloat DxMin = drdx*mesh->Dr[iM*mesh->Np+n] + dsdx*mesh->Ds[iM*mesh->Np+n];
            dfloat DyMin = drdy*mesh->Dr[iM*mesh->Np+n] + dsdy*mesh->Ds[iM*mesh->Np+n];

            // OP11 = OP11 + (- Dn1'*mmE );
            SM[n*mesh->Np+mM] +=  -0.5*nx*(1+bcD)*(1-bcN)*DxMin*MSfim;
            SM[n*mesh->Np+mM] +=  -0.5*ny*(1+bcD)*(1-bcN)*DyMin*MSfim;

            if(eP>=0){
              //OP12(:,Fm2) = OP12(:,Fm2) - 0.5*(-Dn1'*mmE(:, Fm1) );
              SP[n*mesh->Np+mP] +=  +0.5*nx*DxMin*MSfim;
              SP[n*mesh->Np+mP] +=  +0.5*ny*DyMin*MSfim;
            }
          }
        }
      }


      // store non-zeros for off diagonal block
      if(eP>=0){
        for(iint j=0;j<basisNp;++j){
          for(iint i=0;i<basisNp;++i){
            dfloat val = 0;
            for(iint n=0;n<mesh->Np;++n){
              for(iint m=0;m<mesh->Np;++m){
                val += basis[n*mesh->Np+j]*SP[n*mesh->Np+m]*basis[m*mesh->Np+i];
              }
            }

            iint row = globalIds[j + eM*basisNp];
            iint col = globalIds[i + eP*basisNp];
            iint owner = globalOwners[j + eM*basisNp];

            nnz = addNonZero(A, nnz, row, col, owner, val);

          }
        }
      }
    }
    // store non-zeros for diagonal block
    for(iint j=0;j<basisNp;++j){
      for(iint i=0;i<basisNp;++i){
        dfloat val = 0;
        for(iint n=0;n<mesh->Np;++n){
          for(iint m=0;m<mesh->Np;++m){
            val += basis[n*mesh->Np+j]*SM[n*mesh->Np+m]*basis[m*mesh->Np+i];
          }
        }

        iint row = globalIds[j + eM*basisNp];
        iint col = globalIds[i + eM*basisNp];
        iint owner = globalOwners[j + eM*basisNp];

        nnz = addNonZero(A, nnz, row, col, owner, val);

      }
    }
  }

  // sort by row ordering
  qsort(A, nnz, sizeof(nonZero_t), parallelCompareRowColumn);

  // Extract patches from blocksA
  // *  a b c
  // a' * 0 0
  // b' 0 * 0
  // c' 0 0 *

  iint patchNp = mesh->Np*(mesh->Nfaces+1);
  *patchesInvA = (dfloat*) calloc(mesh->Nelements*patchNp*patchNp, sizeof(dfloat));


  for(iint eM=0;eM<mesh->Nelements;++eM){

    dfloat *patchA = patchesInvA[0] + eM*patchNp*patchNp;

    for(iint n=0;n<patchNp;++n){
      patchA[n+patchNp*n] = 1; // make sure diagonal is at least identity
    }

    // diagonal block
    for(iint n=0;n<mesh->Np;++n){
      for(iint m=0;m<mesh->Np;++m){
        iint id = n*patchNp + m;
        iint offset = eM*(mesh->Nfaces+1)*mesh->Np*mesh->Np;
        patchA[id] = blocksA[offset + n*mesh->Np+m];
      }
    }

    // load diagonal blocks
    for(iint f=0;f<mesh->Nfaces;++f){
      iint eP = mesh->EToE[eM*mesh->Nfaces+f];
      if(eP>=0){
        for(iint n=0;n<mesh->Np;++n){
          for(iint m=0;m<mesh->Np;++m){
            iint id = ((f+1)*mesh->Np+n)*patchNp + ((f+1)*mesh->Np+m);
            iint offset = eP*(mesh->Nfaces+1)*mesh->Np*mesh->Np;
            patchA[id] = blocksA[offset + n*mesh->Np+m];
          }
        }
      }
    }

    // load off diagonal blocks ( rely on symmetry )
    for(iint f=0;f<mesh->Nfaces;++f){
      for(iint n=0;n<mesh->Np;++n){
        for(iint m=0;m<mesh->Np;++m){
          iint id = n*patchNp + ((f+1)*mesh->Np+m);
          iint offset = eM*(mesh->Nfaces+1)*mesh->Np*mesh->Np + (f+1)*mesh->Np*mesh->Np;
          patchA[id] = blocksA[offset + n*mesh->Np+m];

          iint idT = n + ((f+1)*mesh->Np+m)*patchNp;
          patchA[idT] = blocksA[offset + n*mesh->Np+m];
        }
      }
    }
    //    printf("------------------\n");
    //    for(iint n=0;n<patchNp;++n){
    //      for(iint m=0;m<patchNp;++m){
    //  if(fabs(patchA[n*patchNp+m])>1e-10)
    //    printf("x");
    //  else
    //        printf("o");
    //      }
    //      printf("\n");
    //    }
    // in place inverse (patchA points into patchesInvA[0])
    matrixInverse(patchNp, patchA);
  }

#if 0
  FILE *fp = fopen("checkGeneralMatrix.m", "w");
  fprintf(fp, "spA = [ \n");
  for(iint n=0;n<*nnzA;++n){
    fprintf(fp, "%d %d %lg;\n", (*A)[n].row+1, (*A)[n].col+1, (*A)[n].val);
  }
  fprintf(fp,"];\n");
  fprintf(fp,"A = spconvert(spA);\n");
  fprintf(fp,"A = full(A);\n");

  fprintf(fp,"error = max(max(A-transpose(A)))\n");
  fclose(fp);
#endif

  free(globalIds);
  free(globalOwners);
  free(A);

  free(blocksA);
  free(MS);
}

void ellipticBuildApproxPatchesIpdgTri2D(mesh2D *mesh, iint basisNp, dfloat *basis,
                                   dfloat tau, dfloat lambda, iint *BCType,
                                   iint *Npatches, iint **patchesIndex, dfloat **patchesInvA,
                                   const char *options){

  iint size, rankM;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rankM);

  if(!basis) { // default to degree N Lagrange basis
    basisNp = mesh->Np;
    basis = (dfloat*) calloc(basisNp*basisNp, sizeof(dfloat));
    for(iint n=0;n<basisNp;++n){
      basis[n+n*basisNp] = 1;
    }
  }

  // number of degrees of freedom on this rank
  iint Nnum = basisNp*mesh->Nelements;

  // create a global numbering system
  iint *globalIds = (iint *) calloc((mesh->Nelements+mesh->totalHaloPairs)*basisNp,sizeof(iint));
  iint *globalOwners = (iint*) calloc(Nnum, sizeof(iint));

  // every degree of freedom has its own global id
  /* so find number of elements on each rank */
  iint *rankNelements = (iint*) calloc(size, sizeof(iint));
  iint *rankStarts = (iint*) calloc(size+1, sizeof(iint));
  MPI_Allgather(&(mesh->Nelements), 1, MPI_IINT,
                rankNelements, 1, MPI_IINT, MPI_COMM_WORLD);
  //find offsets
  for(iint r=0;r<size;++r){
    rankStarts[r+1] = rankStarts[r]+rankNelements[r];
  }
  //use the offsets to set a global id
  for (iint e =0;e<mesh->Nelements;e++) {
    for (int n=0;n<basisNp;n++) {
      globalIds[e*basisNp +n] = n + (e + rankStarts[rankM])*basisNp;
      globalOwners[e*basisNp +n] = rankM;
    }
  }

  /* do a halo exchange of global node numbers */
  if (mesh->totalHaloPairs) {
    iint *idSendBuffer = (iint *) calloc(basisNp*mesh->totalHaloPairs,sizeof(iint));
    meshHaloExchange(mesh, basisNp*sizeof(iint), globalIds, idSendBuffer, globalIds + mesh->Nelements*basisNp);
    free(idSendBuffer);
  }

  iint nnzLocalBound = basisNp*basisNp*(1+mesh->Nfaces)*mesh->Nelements;


  // surface mass matrices MS = MM*LIFT
  dfloat *MS = (dfloat *) calloc(mesh->Nfaces*mesh->Nfp*mesh->Nfp,sizeof(dfloat));
  for (iint f=0;f<mesh->Nfaces;f++) {
    for (iint n=0;n<mesh->Nfp;n++) {
      iint fn = mesh->faceNodes[f*mesh->Nfp+n];

      for (iint m=0;m<mesh->Nfp;m++) {
        dfloat MSnm = 0;

        for (iint i=0;i<mesh->Np;i++){
          MSnm += mesh->MM[fn+i*mesh->Np]*mesh->LIFT[i*mesh->Nfp*mesh->Nfaces+f*mesh->Nfp+m];
        }

        MS[m+n*mesh->Nfp + f*mesh->Nfp*mesh->Nfp]  = MSnm;
      }
    }
  }

  iint blockCount = 0;
  iint patchNp = mesh->Np*(mesh->Nfaces+1);
  iint Nperm = mesh->Nfaces*mesh->Nfaces*mesh->Nfaces;

  // count number of patches
  *Npatches = Nperm;

  for(iint eM=0;eM<mesh->Nelements;++eM){
    iint eP0 = mesh->EToE[eM*mesh->Nfaces+0];
    iint eP1 = mesh->EToE[eM*mesh->Nfaces+1];
    iint eP2 = mesh->EToE[eM*mesh->Nfaces+2];

    if(eP0<0 || eP1<0 || eP2<0){
      ++(*Npatches);
    }
  }

  //  dfloat *permInvA = (dfloat*) calloc(Nperm*patchNp*patchNp, sizeof(dfloat));
  iint *permIndex = (iint*) calloc(patchNp, sizeof(dfloat));
  *patchesInvA = (dfloat*) calloc((*Npatches)*patchNp*patchNp, sizeof(dfloat));

  int blkCounter = 0;



  for(iint blk=0;blk<Nperm;++blk){
    iint f0 = blk%mesh->Nfaces;
    iint f1 = (blk/mesh->Nfaces)%mesh->Nfaces;
    iint f2= (blk/(mesh->Nfaces*mesh->Nfaces));

#if 1
    iint r0 = (f0==0) ? 0: mesh->Nfaces-f0;
    iint r1 = (f1==0) ? 0: mesh->Nfaces-f1;
    iint r2 = (f2==0) ? 0: mesh->Nfaces-f2;
#else
    iint r0 = f0;
    iint r1 = f1;
    iint r2 = f2;
#endif

    for(iint n=0;n<mesh->Np;++n){
      permIndex[n+0*mesh->Np] = 0*mesh->Np + n;
      permIndex[n+1*mesh->Np] = 1*mesh->Np + mesh->rmapP[r0*mesh->Np+n];
      permIndex[n+2*mesh->Np] = 2*mesh->Np + mesh->rmapP[r1*mesh->Np+n];
      permIndex[n+3*mesh->Np] = 3*mesh->Np + mesh->rmapP[r2*mesh->Np+n];
    }
#if 0
    for(iint n=0;n<patchNp;++n){
      printf("[%d] ", permIndex[n]);
    }
    printf("\n");
#endif

    for(iint n=0;n<patchNp;++n){
      for(iint m=0;m<patchNp;++m){
      	iint pn = permIndex[n];
      	iint pm = permIndex[m];
      	(*patchesInvA)[blkCounter*patchNp*patchNp + n*patchNp + m] = mesh->invAP[pn*patchNp+pm]; // maybe need to switch map
      }
    }
    ++blkCounter;
  }

  // reset non-zero counter
  int nnz = 0;

  nonZero_t *A = (nonZero_t*) calloc(nnzLocalBound, sizeof(nonZero_t));

  // TW: ONLY NON-MPI FOR THE MOMENT
  dfloat *blocksA = (dfloat *) calloc(mesh->Nelements*(mesh->Nfaces+1)*mesh->Np*mesh->Np,sizeof(dfloat));

  // loop over all elements
  for(iint eM=0;eM<mesh->Nelements;++eM){

    dfloat *SM = blocksA + eM*(mesh->Nfaces+1)*mesh->Np*mesh->Np;

    iint vbase = eM*mesh->Nvgeo;
    dfloat drdx = mesh->vgeo[vbase+RXID];
    dfloat drdy = mesh->vgeo[vbase+RYID];
    dfloat dsdx = mesh->vgeo[vbase+SXID];
    dfloat dsdy = mesh->vgeo[vbase+SYID];
    dfloat J = mesh->vgeo[vbase+JID];

    /* start with stiffness matrix  */
    for(iint n=0;n<mesh->Np;++n){
      for(iint m=0;m<mesh->Np;++m){
        SM[n*mesh->Np+m]  = J*lambda*mesh->MM[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*drdx*drdx*mesh->Srr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*drdx*dsdx*mesh->Srs[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdx*drdx*mesh->Ssr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdx*dsdx*mesh->Sss[n*mesh->Np+m];

        SM[n*mesh->Np+m] += J*drdy*drdy*mesh->Srr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*drdy*dsdy*mesh->Srs[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdy*drdy*mesh->Ssr[n*mesh->Np+m];
        SM[n*mesh->Np+m] += J*dsdy*dsdy*mesh->Sss[n*mesh->Np+m];
      }
    }

    for (iint fM=0;fM<mesh->Nfaces;fM++) {

      dfloat *SP = blocksA + (fM+1)*mesh->Np*mesh->Np + eM*mesh->Np*mesh->Np*(mesh->Nfaces+1);

      // load surface geofactors for this face
      iint sid = mesh->Nsgeo*(eM*mesh->Nfaces+fM);
      dfloat nx = mesh->sgeo[sid+NXID];
      dfloat ny = mesh->sgeo[sid+NYID];
      dfloat sJ = mesh->sgeo[sid+SJID];
      dfloat hinv = mesh->sgeo[sid+IHID];
      dfloat penalty = tau*hinv;

      iint eP = mesh->EToE[eM*mesh->Nfaces+fM];
      if (eP < 0) eP = eM;

      iint vbaseP = eP*mesh->Nvgeo;
      dfloat drdxP = mesh->vgeo[vbaseP+RXID];
      dfloat drdyP = mesh->vgeo[vbaseP+RYID];
      dfloat dsdxP = mesh->vgeo[vbaseP+SXID];
      dfloat dsdyP = mesh->vgeo[vbaseP+SYID];

      int bcD = 0, bcN =0;
      int bc = mesh->EToB[fM+mesh->Nfaces*eM]; //raw boundary flag
      iint bcType = 0;

      if(bc>0) bcType = BCType[bc];          //find its type (Dirichlet/Neumann)

      // this needs to be double checked (and the code where these are used)
      if(bcType==1){ // Dirichlet
        bcD = 1;
        bcN = 0;
      } else if(bcType==2){ // Neumann
        bcD = 0;
        bcN = 1;
      }

      // reset eP
      eP = mesh->EToE[eM*mesh->Nfaces+fM];

      // mass matrix for this face
      dfloat *MSf = MS+fM*mesh->Nfp*mesh->Nfp;

      // penalty term just involves face nodes
      for(iint n=0;n<mesh->Nfp;++n){
        for(iint m=0;m<mesh->Nfp;++m){
          iint nM = mesh->faceNodes[fM*mesh->Nfp+n];
          iint mM = mesh->faceNodes[fM*mesh->Nfp+m];

          // OP11 = OP11 + 0.5*( gtau*mmE )
          dfloat MSfnm = sJ*MSf[n*mesh->Nfp+m];
          SM[nM*mesh->Np+mM] += 0.5*(1.-bcN)*(1.+bcD)*penalty*MSfnm;

          // neighbor penalty term
          if(eP>=0){
            iint idM = eM*mesh->Nfp*mesh->Nfaces+fM*mesh->Nfp+m;
            iint mP  = mesh->vmapP[idM]%mesh->Np;

            // OP12(:,Fm2) = - 0.5*( gtau*mmE(:,Fm1) );
            SP[nM*mesh->Np+mP] += -0.5*penalty*MSfnm;
          }
        }
      }

      // now add differential surface terms
      for(iint n=0;n<mesh->Nfp;++n){
        for(iint m=0;m<mesh->Np;++m){
          iint nM = mesh->faceNodes[fM*mesh->Nfp+n];

          for(iint i=0;i<mesh->Nfp;++i){
            iint iM = mesh->faceNodes[fM*mesh->Nfp+i];
            iint iP = mesh->vmapP[i + fM*mesh->Nfp+eM*mesh->Nfp*mesh->Nfaces]%mesh->Np;

            dfloat MSfni = sJ*MSf[n*mesh->Nfp+i]; // surface Jacobian built in

            dfloat DxMim = drdx*mesh->Dr[iM*mesh->Np+m] + dsdx*mesh->Ds[iM*mesh->Np+m];
            dfloat DyMim = drdy*mesh->Dr[iM*mesh->Np+m] + dsdy*mesh->Ds[iM*mesh->Np+m];

            // OP11 = OP11 + 0.5*( - mmE*Dn1)
            SM[nM*mesh->Np+m] += -0.5*nx*(1+bcD)*(1-bcN)*MSfni*DxMim;
            SM[nM*mesh->Np+m] += -0.5*ny*(1+bcD)*(1-bcN)*MSfni*DyMim;

            if(eP>=0){

              dfloat DxPim = drdxP*mesh->Dr[iP*mesh->Np+m] + dsdxP*mesh->Ds[iP*mesh->Np+m];
              dfloat DyPim = drdyP*mesh->Dr[iP*mesh->Np+m] + dsdyP*mesh->Ds[iP*mesh->Np+m];

              //OP12(Fm1,:) = OP12(Fm1,:) - 0.5*(      mmE(Fm1,Fm1)*Dn2(Fm2,:) );
              SP[nM*mesh->Np+m] += -0.5*nx*MSfni*DxPim;
              SP[nM*mesh->Np+m] += -0.5*ny*MSfni*DyPim;
            }
          }
        }
      }

      for(iint n=0;n<mesh->Np;++n){
        for(iint m=0;m<mesh->Nfp;++m){
          iint mM = mesh->faceNodes[fM*mesh->Nfp+m];
          iint mP = mesh->vmapP[m + fM*mesh->Nfp+eM*mesh->Nfp*mesh->Nfaces]%mesh->Np;

          for(iint i=0;i<mesh->Nfp;++i){
            iint iM = mesh->faceNodes[fM*mesh->Nfp+i];

            dfloat MSfim = sJ*MSf[i*mesh->Nfp+m];

            dfloat DxMin = drdx*mesh->Dr[iM*mesh->Np+n] + dsdx*mesh->Ds[iM*mesh->Np+n];
            dfloat DyMin = drdy*mesh->Dr[iM*mesh->Np+n] + dsdy*mesh->Ds[iM*mesh->Np+n];

            // OP11 = OP11 + (- Dn1'*mmE );
            SM[n*mesh->Np+mM] +=  -0.5*nx*(1+bcD)*(1-bcN)*DxMin*MSfim;
            SM[n*mesh->Np+mM] +=  -0.5*ny*(1+bcD)*(1-bcN)*DyMin*MSfim;

            if(eP>=0){
              //OP12(:,Fm2) = OP12(:,Fm2) - 0.5*(-Dn1'*mmE(:, Fm1) );
              SP[n*mesh->Np+mP] +=  +0.5*nx*DxMin*MSfim;
              SP[n*mesh->Np+mP] +=  +0.5*ny*DyMin*MSfim;
            }
          }
        }
      }


      // store non-zeros for off diagonal block
      if(eP>=0){
        for(iint j=0;j<basisNp;++j){
          for(iint i=0;i<basisNp;++i){
            dfloat val = 0;
            for(iint n=0;n<mesh->Np;++n){
              for(iint m=0;m<mesh->Np;++m){
                val += basis[n*mesh->Np+j]*SP[n*mesh->Np+m]*basis[m*mesh->Np+i];
              }
            }

            iint row = globalIds[j + eM*basisNp];
            iint col = globalIds[i + eP*basisNp];
            iint owner = globalOwners[j + eM*basisNp];

            nnz = addNonZero(A, nnz, row, col, owner, val);

          }
        }
      }
    }
    // store non-zeros for diagonal block
    for(iint j=0;j<basisNp;++j){
      for(iint i=0;i<basisNp;++i){
        dfloat val = 0;
        for(iint n=0;n<mesh->Np;++n){
          for(iint m=0;m<mesh->Np;++m){
            val += basis[n*mesh->Np+j]*SM[n*mesh->Np+m]*basis[m*mesh->Np+i];
          }
        }

        iint row = globalIds[j + eM*basisNp];
        iint col = globalIds[i + eM*basisNp];
        iint owner = globalOwners[j + eM*basisNp];

        nnz = addNonZero(A, nnz, row, col, owner, val);

      }
    }
  }

  // sort by row ordering
  qsort(A, nnz, sizeof(nonZero_t), parallelCompareRowColumn);

  // Extract patches from blocksA
  // *  a b c
  // a' * 0 0
  // b' 0 * 0
  // c' 0 0 *
  int refPatches = 0;

  *patchesIndex = (iint*) calloc(mesh->Nelements, sizeof(iint));

  for(iint eM=0;eM<mesh->Nelements;++eM){

    iint eP0 = mesh->EToE[eM*mesh->Nfaces+0];
    iint eP1 = mesh->EToE[eM*mesh->Nfaces+1];
    iint eP2 = mesh->EToE[eM*mesh->Nfaces+2];

    iint fP0 = mesh->EToF[eM*mesh->Nfaces+0];
    iint fP1 = mesh->EToF[eM*mesh->Nfaces+1];
    iint fP2 = mesh->EToF[eM*mesh->Nfaces+2];

#if 1
    if(eP0>=0 && eP1>=0 && eP2>=0){
      iint blk = fP0 + mesh->Nfaces*fP1 + mesh->Nfaces*mesh->Nfaces*fP2;

      ++refPatches;
      (*patchesIndex)[eM] = blk;
    } else
#endif
    {
    	dfloat *patchA = patchesInvA[0] + blkCounter*patchNp*patchNp;

    	for(iint n=0;n<patchNp;++n){
    	  patchA[n+patchNp*n] = 1; // make sure diagonal is at least identity
    	}

    	// diagonal block
    	for(iint n=0;n<mesh->Np;++n){
    	  for(iint m=0;m<mesh->Np;++m){
    	    iint id = n*patchNp + m;
    	    iint offset = eM*(mesh->Nfaces+1)*mesh->Np*mesh->Np;
    	    patchA[id] = blocksA[offset + n*mesh->Np+m];
    	  }
    	}

    	// load diagonal blocks
    	for(iint f=0;f<mesh->Nfaces;++f){
    	  iint eP = mesh->EToE[eM*mesh->Nfaces+f];
    	  if(eP>=0){
    	    for(iint n=0;n<mesh->Np;++n){
    	      for(iint m=0;m<mesh->Np;++m){
          		iint id = ((f+1)*mesh->Np+n)*patchNp + ((f+1)*mesh->Np+m);
          		iint offset = eP*(mesh->Nfaces+1)*mesh->Np*mesh->Np;
          		patchA[id] = blocksA[offset + n*mesh->Np+m];
    	      }
    	    }
    	  }
    	}

    	// load off diagonal blocks ( rely on symmetry )
    	for(iint f=0;f<mesh->Nfaces;++f){
    	  for(iint n=0;n<mesh->Np;++n){
    	    for(iint m=0;m<mesh->Np;++m){
    	      iint id = n*patchNp + ((f+1)*mesh->Np+m);
    	      iint offset = eM*(mesh->Nfaces+1)*mesh->Np*mesh->Np + (f+1)*mesh->Np*mesh->Np;
    	      patchA[id] = blocksA[offset + n*mesh->Np+m];

    	      iint idT = n + ((f+1)*mesh->Np+m)*patchNp;
    	      patchA[idT] = blocksA[offset + n*mesh->Np+m];
    	    }
    	  }
    	}
    	//    printf("------------------\n");
    	//    for(iint n=0;n<patchNp;++n){
    	//      for(iint m=0;m<patchNp;++m){
    	//	if(fabs(patchA[n*patchNp+m])>1e-10)
    	//	  printf("x");
    	//	else
    	//    	  printf("o");
    	//      }
    	//      printf("\n");
    	//    }
    	// in place inverse (patchA points into patchesInvA[0])
    	matrixInverse(patchNp, patchA);

    	(*patchesIndex)[eM] = blkCounter;
    	++blkCounter;
    }
  }

  printf("using %d reference patches\n", refPatches);

#if 0
  FILE *fp = fopen("checkGeneralMatrix.m", "w");
  fprintf(fp, "spA = [ \n");
  for(iint n=0;n<*nnzA;++n){
    fprintf(fp, "%d %d %lg;\n", (*A)[n].row+1, (*A)[n].col+1, (*A)[n].val);
  }
  fprintf(fp,"];\n");
  fprintf(fp,"A = spconvert(spA);\n");
  fprintf(fp,"A = full(A);\n");

  fprintf(fp,"error = max(max(A-transpose(A)))\n");
  fclose(fp);
#endif

  free(globalIds);
  free(globalOwners);
  free(A);

  free(blocksA);
  free(MS);
}

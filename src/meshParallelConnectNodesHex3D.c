#include <stdio.h>
#include <stdlib.h>
#include "mpi.h"
#include "mesh3D.h"

// iteratively find a global numbering for all local element nodes
void meshParallelConnectNodesHex3D(mesh3D *mesh){

  int rank, size;

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  // form continuous node numbering (local=>virtual global)
  iint *globalNumbering = (iint*) calloc((mesh->totalHaloPairs+mesh->Nelements)*mesh->Np, sizeof(iint));
  iint *globalBoundaryTags = (iint*) calloc((mesh->totalHaloPairs+mesh->Nelements)*mesh->Np, sizeof(iint));
  
  // assume ordering (brittle)
  iint vnums[8];

  vnums[0] = 0;
  vnums[1] = mesh->N;
  vnums[2] = (mesh->N+1)*(mesh->N+1)-1;
  vnums[3] = mesh->N*(mesh->N+1); 

  vnums[4] = mesh->Nq*mesh->Nq*mesh->N + 0;
  vnums[5] = mesh->Nq*mesh->Nq*mesh->N + mesh->N;
  vnums[6] = mesh->Np-1;
  vnums[7] = mesh->Nq*mesh->Nq*mesh->N + mesh->N*mesh->Nq;

  printf("vnums=\n");
  for(iint n=0;n<mesh->Nverts;++n){
    printf("%d ", vnums[n]);
  }
  printf("\n");
  
  // use local numbering 
  for(iint n=0;n<mesh->Nelements*mesh->Np;++n){
    globalNumbering[n] = 1+n+mesh->Nnodes;
  }
  
  // use vertex ids for vertex nodes to reduce iterations
  for(iint e=0;e<mesh->Nelements;++e){
    for(iint v=0;v<mesh->Nverts;++v){
      iint lid = e*mesh->Np + vnums[v];
      iint gid = mesh->EToV[e*mesh->Nverts+v] + 1;
      globalNumbering[lid] = gid;
    }
  }

  // use element-to-boundary connectivity to create tag for local nodes
  for(iint e=0;e<mesh->Nelements;++e){
    for(iint f=0;f<mesh->Nfaces;++f){
      for(iint n=0;n<mesh->Nfp;++n){
	iint lid = mesh->faceNodes[f*mesh->Nfp+n];
	iint tag = mesh->EToB[e*mesh->Nfaces+f];
	if(tag>0)
	  globaBoundaryTags[e*mesh->Np + lid] = tag;
      }
    }
  }

  iint localChange = 0, globalChange = 1;

  iint *sendBuffer = (iint*) calloc(mesh->totalHaloPairs*mesh->Np, sizeof(iint));

  printf("totalHaloPairs = %d\n", mesh->totalHaloPairs);
  
  while(globalChange>0){

    // reset change counter
    localChange = 0;

    // extract halo
    
    // send halo data and recv into extension of buffer
    meshHaloExchange3D(mesh, mesh->Np*sizeof(iint), globalNumbering, s   endBuffer, globalNumbering   +mesh->Np*mesh->Nelements);
    meshHaloExchange3D(mesh, mesh->Np*sizeof(iint), globalBoundaryTags, sendBuffer, globalBoundaryTags+mesh->Np*mesh->Nelements);

    // compare trace nodes
    for(iint e=0;e<mesh->Nelements;++e){
      for(iint n=0;n<mesh->Nfp*mesh->Nfaces;++n){
	iint id  = e*mesh->Nfp*mesh->Nfaces + n;
	iint idM = mesh->vmapM[id];
	iint idP = mesh->vmapP[id];
	iint gidM = globalNumbering[idM];
	iint gidP = globalNumbering[idP];

	// use minimum of trace variables
	if(gidM!=gidP){
	  ++localChange;
	  globalNumbering[idM] = mymax(gidP,gidM);
	  globalNumbering[idP] = mymax(gidP,gidM);
	}

	iint tagM = globalBoundaryTags[idM];
	iint tagP = globalBoundaryTags[idM];

	// use minimum non-zer tag for both nodes
	if(tagM!=tagP){
	  ++localChange;
	  if(tagM>0 || tagP>0){
	    if(tagP>tagM && tagM>0) tagP = tagM;
	    if(tagM>tagP && tagP>0) tagM = tagP;
	    globalBoundaryTags[idM] = tagM;
	    globalBoundaryTags[idP] = tagP;
	  }
	}
      }
    }

    // sum up changes
    MPI_Allreduce(&localChange, &globalChange, 1, MPI_IINT, MPI_SUM, MPI_COMM_WORLD);

    // report
    if(rank==0)
      printf("globalChange=%d\n", globalChange);
  }

  // should do something with tag and global numbering arrays
  free(sendBuffer);

}

#include "boltzmannTri3D.h"

int main(int argc, char **argv){

  // start up MPI
  MPI_Init(&argc, &argv);

  if(argc!=3){
    // to run cavity test case with degree N elements
    printf("usage: ./main meshes/cavityH005.msh N\n");
    exit(-1);
  }

  // int specify polynomial degree 
  int N = atoi(argv[2]);

  // set up mesh stuff
  dfloat sphereRadius = 1;
  mesh_t *mesh = meshSetupTri3D(argv[1], N, sphereRadius);

  // set up boltzmann stuff
  solver_t *solver = boltzmannSetupTri3D(mesh);

  // time step Boltzmann equations
  boltzmannRunTri3D(solver);

  // close down MPI
  MPI_Finalize();

  exit(0);
  return 0;
}

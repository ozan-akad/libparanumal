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

#include "ellipticQuad2D.h"

void ellipticPreconditioner2D(solver_t *solver,
            dfloat lambda,
            occa::memory &o_r,
            occa::memory &o_z,
            const char *options){

  mesh_t *mesh = solver->mesh;
  precon_t *precon = solver->precon;

  if (strstr(options, "FULLALMOND")||strstr(options, "MULTIGRID")||strstr(options, "SEMFEM")) {

    occaTimerTic(mesh->device,"parALMOND");
    parAlmondPrecon(precon->parAlmond, o_z, o_r);
    occaTimerToc(mesh->device,"parALMOND");

  } else if(strstr(options, "OAS")){
    
    //patch solve
    //ellipticPatchSmootherQuad2D(solver,o_r,o_z,options);
    smoothQuad2D(precon->OASsmoothArgs, o_r, o_z,true);

    occaTimerTic(mesh->device,"coarseGrid");

    // Z1*Z1'*PL1*(Z1*z1) = (Z1*rL)  HMMM
    occaTimerTic(mesh->device,"coarsenKernel");
    precon->coarsenKernel(mesh->Nelements, precon->o_V1, o_r, precon->o_r1);
    occaTimerToc(mesh->device,"coarsenKernel");

    occaTimerTic(mesh->device,"ALMOND");
    parAlmondPrecon(precon->parAlmond, precon->o_z1, precon->o_r1);
    occaTimerToc(mesh->device,"ALMOND");

    // prolongate from P1 to PN, adding patch and coarse solves together
    occaTimerTic(mesh->device,"prolongateKernel");
    precon->prolongateKernel(mesh->Nelements, precon->o_V1, precon->o_z1, solver->o_z);
    occaTimerToc(mesh->device,"prolongateKernel");

    occaTimerToc(mesh->device,"coarseGrid");

  } else if(strstr(options, "JACOBI")){

    dlong Ntotal = mesh->Np*mesh->Nelements;
    // Jacobi preconditioner
    occaTimerTic(mesh->device,"dotDivideKernel");
    solver->dotMultiplyKernel(Ntotal, o_r, precon->o_invDiagA, o_z);
    occaTimerToc(mesh->device,"dotDivideKernel");
  
  }  else{ // turn off preconditioner
    o_z.copyFrom(o_r); 
  }
}

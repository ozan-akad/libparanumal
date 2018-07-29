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

#include "mns.h"

// complete a time step using LSERK4
void mnsComputeSignumTerm(mns_t *mns, int haloBytes, dfloat * sendBuffer, dfloat *recvBuffer){

  mesh_t *mesh = mns->mesh; 

  dfloat time = 0.0; 

  if(mesh->totalHaloPairs>0){
    #if ASYNC 
      mesh->device.setStream(dataStream);
    #endif

    mesh->haloExtractKernel(mesh->totalHaloPairs,
                            mesh->Np,
                            mesh->o_haloElementList,
                            mns->o_Phi,
                            mesh->o_haloBuffer);

    // copy extracted halo to HOST
    mesh->o_haloBuffer.asyncCopyTo(sendBuffer);

    #if ASYNC 
      mesh->device.setStream(defaultStream);
    #endif
  }


  // Compute Gradient volume Kernel
  occaTimerTic(mesh->device,"GradientVolume");
  // Compute Volume Contribution
  mns->gradientVolumeKernel(mesh->Nelements,
                            mesh->o_vgeo,
                            mesh->o_Dmatrices,
                            mns->fieldOffset,
                            mns->o_Phi,
                            mns->o_GPhi);
  occaTimerToc(mesh->device,"GradientVolume");



  if(mesh->totalHaloPairs>0){

    #if ASYNC 
      mesh->device.setStream(dataStream);
    #endif

    //make sure the async copy is finished
    mesh->device.finish();
    // start halo exchange
    meshHaloExchangeStart(mesh,
                          mesh->Np*sizeof(dfloat),
                          sendBuffer,
                          recvBuffer);
    // wait for halo data to arrive
    meshHaloExchangeFinish(mesh);
    // copy halo data to DEVICE
    size_t offset = mesh->Np*mesh->Nelements*sizeof(dfloat); // offset for halo data
    mns->o_Phi.asyncCopyFrom(recvBuffer, haloBytes, offset);
    mesh->device.finish();        

    #if ASYNC 
      mesh->device.setStream(defaultStream);
    #endif
  }


  occaTimerTic(mesh->device,"GradientSurface");
  // Compute Surface Conribution
  mns->gradientSurfaceKernel(mesh->Nelements,
                            mesh->o_sgeo,
                            mesh->o_LIFTT,
                            mesh->o_vmapM,
                            mesh->o_vmapP,
                            mesh->o_EToB,
                            mesh->o_x,
                            mesh->o_y,
                            mesh->o_z,
                            time,
                            mns->fieldOffset,
                            mns->o_Phi,
                            mns->o_GPhi);
  occaTimerToc(mesh->device,"GradientSurface");


 
  occaTimerTic(mesh->device,"RegularizedSignum");
  mns->regularizedSignumKernel(mesh->Nelements,
                              mns->hmin,
                              mns->fieldOffset,
                              mns->o_Phi,
                              mns->o_GPhi,
                              mns->o_SPhi);

  occaTimerToc(mesh->device,"RegularizedSignum");



}
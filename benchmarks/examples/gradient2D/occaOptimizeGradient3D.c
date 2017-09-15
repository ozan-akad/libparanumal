
#include "occa.hpp"
#include "mesh3D.h"

void occaOptimizeGradient3D(mesh3D *mesh, dfloat *q, dfloat *dqdx, dfloat *dqdy, dfloat *dqdz){

  // print all devices 
  occa::printAvailableDevices();

  // build OCCA device
  occa::device device;

  //---[ Device setup with string flags ]-------------------
  device.setup("mode = CUDA, deviceID = 0");
  //device.setup("mode = OpenCL  , platformID = 0, deviceID = 1");
  //device.setup("mode = OpenMP");

  // set up compiler flags
  occa::kernelInfo info;
  // add definition for some compiler variables
  info.addDefine("p_Np", mesh->Np);
  info.addDefine("p_Nvgeo", mesh->Nvgeo);
  info.addDefine("p_Nthreads", 128); // caution - chose 256 since OpenCL only supports up to 256 inner iterations
  if(sizeof(dfloat)==4){
    info.addDefine("dfloat","float");
    info.addDefine("dfloat4","float4");
  }
  if(sizeof(dfloat)==8){
    info.addDefine("dfloat","double");
    info.addDefine("dfloat4","double4");
  }
  info.addDefine("p_Nblock", 256/mesh->Np); // get close to 256/Np elements for the inner part of v9 kernel
  

#if 0
  info.addCompilerFlag("--ftz=true");
  info.addCompilerFlag("--prec-div=false");
  info.addCompilerFlag("--prec-sqrt=false");
  info.addCompilerFlag("--use_fast_math");
  info.addCompilerFlag("--fmad=true"); // compiler option for cuda
#endif

  /* build set of kernels to test */
#define maxNkernels 100
  int Nkernels = 10;
  occa::kernel meshGradient3DKernels[maxNkernels];
  char kernelNames[maxNkernels][BUFSIZ];
  for(int ker=0;ker<Nkernels;++ker){
    sprintf(kernelNames[ker], "meshGradient3D_v%d", ker);
    
    meshGradient3DKernels[ker] =
      device.buildKernelFromSource("src/meshOptimizedGradient3D.okl", kernelNames[ker], info);
  }
  
  // allocate DEVICE arrays
  occa::memory o_q    = device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat), q);
  occa::memory o_dqdx = device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat), dqdx);
  occa::memory o_dqdy = device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat), dqdy);
  occa::memory o_dqdz = device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat), dqdz);
  occa::memory o_vgeo = 
    device.malloc(mesh->Nvgeo*mesh->Nelements*sizeof(dfloat), mesh->vgeo);

  dfloat *q4 = (dfloat*) calloc(4*mesh->Np*mesh->Nelements, sizeof(dfloat));
  dfloat *dq4dx = (dfloat*) calloc(4*mesh->Np*mesh->Nelements, sizeof(dfloat));
  dfloat *dq4dy = (dfloat*) calloc(4*mesh->Np*mesh->Nelements, sizeof(dfloat));
  dfloat *dq4dz = (dfloat*) calloc(4*mesh->Np*mesh->Nelements, sizeof(dfloat));

  occa::memory o_q4  = 
    device.malloc(4*mesh->Np*mesh->Nelements*sizeof(dfloat), q4);
  occa::memory o_dq4dx  = 
    device.malloc(4*mesh->Np*mesh->Nelements*sizeof(dfloat), dq4dx);
  occa::memory o_dq4dy  = 
    device.malloc(4*mesh->Np*mesh->Nelements*sizeof(dfloat), dq4dy);
  occa::memory o_dq4dz  = 
    device.malloc(4*mesh->Np*mesh->Nelements*sizeof(dfloat), dq4dz);
  
  // create transposes of Dr and Ds
  dfloat *DrT = (dfloat*) calloc(mesh->Np*mesh->Np, sizeof(dfloat));
  dfloat *DsT = (dfloat*) calloc(mesh->Np*mesh->Np, sizeof(dfloat));
  dfloat *DtT = (dfloat*) calloc(mesh->Np*mesh->Np, sizeof(dfloat));
  for(int n=0;n<mesh->Np;++n){
    for(int m=0;m<mesh->Np;++m){
      DrT[n+m*mesh->Np] = mesh->Dr[n*mesh->Np+m];
      DsT[n+m*mesh->Np] = mesh->Ds[n*mesh->Np+m];
      DtT[n+m*mesh->Np] = mesh->Dt[n*mesh->Np+m];
    }
  }
      
  // allocate operator matrices
  occa::memory o_Dr  = device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), mesh->Dr);
  occa::memory o_Ds  = device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), mesh->Ds);
  occa::memory o_Dt  = device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), mesh->Dt);
  occa::memory o_DrT = device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), DrT);
  occa::memory o_DsT = device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), DsT);
  occa::memory o_DtT = device.malloc(mesh->Np*mesh->Np*sizeof(dfloat), DtT);

  int Ntests = 5;

  occa::tic("reference serial code");
  for(int test=0;test<Ntests;++test)
    meshGradient3D(mesh, q, dqdx, dqdy, dqdz);
  occa::toc("reference serial code");
  
  // run each kernel 5 times
  for(int ker=Nkernels-1;ker>=0;--ker){
    device.finish();
    occa::tic(kernelNames[ker]);
    for(int test=0;test<Ntests;++test){
      if(ker<4)
	meshGradient3DKernels[ker](mesh->Nelements, mesh->Np, mesh->Nvgeo, o_vgeo,
				   o_Dr, o_Ds, o_Dt, o_q, o_dqdx, o_dqdy, o_dqdz);
      else if(ker<8)
	meshGradient3DKernels[ker](mesh->Nelements, mesh->Np, mesh->Nvgeo, o_vgeo,
				   o_DrT, o_DsT, o_DtT, o_q, o_dqdx, o_dqdy, o_dqdz);
      else
	meshGradient3DKernels[ker](mesh->Nelements, mesh->Np, mesh->Nvgeo, o_vgeo,
				   o_DrT, o_DsT, o_DtT, o_q4, o_dq4dx, o_dq4dy, o_dq4dz);
    }
    device.finish();
    occa::toc(kernelNames[ker]);
  }
  
  // copy from DEVICE to HOST array
  o_dqdx.copyTo(dqdx);
  o_dqdy.copyTo(dqdy);
  o_dqdz.copyTo(dqdz);

  // print timings
  occa::printTimer();
}




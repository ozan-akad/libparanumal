#include <iostream>
#include "occa.hpp"

#define dfloat float
#define dfloatString "float"

// assumes dfloats
dfloat *randCalloc(int N, size_t sz){
 
  dfloat *pt = (dfloat*) malloc(N*sz);
  for(int n=0;n<N;++n){
    pt[n] = drand48();
  }

  return pt; 
}

// make; OCCA_CUDA_COMPILER_FLAGS='--use_fast_math' OCCA_PROFILE=1 OCCA_KERNEL_PROFILE=1 ./main 8 50000

int main(int argc, char **argv){
  occa::printAvailableDevices();

  int Nq = atoi(argv[1]);
  int K  = atoi(argv[2]);
  int Kblk = atoi(argv[3]);

  K = Kblk*((K+Kblk-1)/Kblk);

  printf("Nq = %d, K = %d, Kblk = %d\n", Nq, K, Kblk); 
	   
  
  int Nfaces = 6;
  int Nsgeo = 6;
  int Nvgeo = 6;
  
  dfloat *D     = (dfloat*) randCalloc(Nq*Nq,sizeof(dfloat));
  dfloat *vgeo  = (dfloat*) randCalloc(Nq*Nq*Nq*Nvgeo*K,sizeof(dfloat));
  dfloat *sgeo  = (dfloat*) randCalloc(Nq*Nq*Nfaces*Nsgeo*K,sizeof(dfloat));
  dfloat *q     = (dfloat*) randCalloc(Nq*Nq*Nq*K,sizeof(dfloat));
  dfloat *qM    = (dfloat*) randCalloc(Nq*Nq*Nfaces*K,sizeof(dfloat));
  dfloat *qP    = (dfloat*) randCalloc(Nq*Nq*Nfaces*K,sizeof(dfloat));
  dfloat *dqdnM = (dfloat*) randCalloc(Nq*Nq*Nfaces*K,sizeof(dfloat));
  dfloat *dqdnP = (dfloat*) randCalloc(Nq*Nq*Nfaces*K,sizeof(dfloat));
  dfloat *Aq    = (dfloat*) randCalloc(Nq*Nq*Nq*K,sizeof(dfloat));

  dfloat *VXYZ  = (dfloat*) randCalloc(24*K, sizeof(dfloat));
  dfloat *gz    = (dfloat*) randCalloc(Nq, sizeof(dfloat));
  

  occa::device device;
  occa::memory o_D, o_vgeo, o_sgeo, o_gz, o_VXYZ;
  occa::memory o_q, o_qM, o_qP, o_dqdnM, o_dqdnP, o_Aq;

  //---[ Device setup with string flags ]-------------------
  //device.setup("mode = Serial");
  // device.setup("mode = OpenMP  , schedule = compact, chunk = 10");
  //device.setup("mode = OpenCL  , platformID = 0, deviceID = 1");
  device.setup("mode = CUDA    , deviceID = 1");
  // device.setup("mode = Pthreads, threadCount = 4, schedule = compact, pinnedCores = [0, 0, 1, 1]");
  // device.setup("mode = COI     , deviceID = 0");
  //========================================================

  o_D     = device.malloc(Nq*Nq*sizeof(dfloat), D);
  o_vgeo  = device.malloc(Nq*Nq*Nq*Nvgeo*K*sizeof(dfloat), vgeo);
  o_sgeo  = device.malloc(Nq*Nq*Nfaces*Nsgeo*K*sizeof(dfloat), sgeo);
  o_q     = device.malloc(Nq*Nq*Nq*K*sizeof(dfloat), q);
  o_qM    = device.malloc(Nq*Nq*Nfaces*K*sizeof(dfloat), qM);
  o_qP    = device.malloc(Nq*Nq*Nfaces*K*sizeof(dfloat), qP);
  o_dqdnM = device.malloc(Nq*Nq*Nfaces*K*sizeof(dfloat), dqdnM);
  o_dqdnP = device.malloc(Nq*Nq*Nfaces*K*sizeof(dfloat), dqdnP);
  o_Aq    = device.malloc(Nq*Nq*Nq*K*sizeof(dfloat), Aq);
  o_gz    = device.malloc(Nq*sizeof(dfloat), gz);
  o_VXYZ  = device.malloc(8*3*K*sizeof(dfloat), VXYZ);

  // info for compilation
  occa::kernelInfo info;
  info.addDefine("p_Nq", Nq);
  info.addDefine("p_Nq2", Nq*Nq);
  info.addDefine("p_Nq3", Nq*Nq*Nq);
  info.addDefine("p_Nfaces", Nfaces);
  info.addDefine("p_Nsgeo", Nsgeo);
  info.addDefine("p_Nvgeo", Nvgeo);
  info.addDefine("p_idsJ", 3);
  info.addDefine("p_idtau", 4);
  info.addDefine("dfloat", dfloatString);
  info.addDefine("p_Kblk", Kblk);

  // OKL: OCCA Kernel Language
#if 0
  occa::kernel scalarEllipticOp = 
        device.buildKernelFromSource("okl/ScalarEllipticSlicedBlockedGeoOnDemandOperator3D.okl",
				 "ScalarEllipticSlicedOperator3D", 
				 info);
#endif
#if 1
  occa::kernel scalarEllipticOp = 
    device.buildKernelFromSource("okl/ScalarEllipticOperator3D.okl",
				 "ScalarEllipticOperator3D", 
				 info);
#endif
#if 0
  // on demand geo slow down 
  occa::kernel scalarEllipticOp = 
    device.buildKernelFromSource("okl/ScalarEllipticGeoOnDemand.okl",
				 "ScalarEllipticOperator3D", 
				 info);
#endif
    
  occa::initTimer(device);
  occa::tic("scalarEllipticOp");
  
  int Nit = 100;
  for(int it=0;it<Nit;++it){
    //    scalarEllipticOp(K, o_D, o_gz, o_VXYZ, o_sgeo, o_q, o_qM, o_qP, o_dqdnM, o_dqdnP, o_Aq);// on demand geo
    scalarEllipticOp(K, o_D, o_vgeo, o_sgeo, o_q, o_qM, o_qP, o_dqdnM, o_dqdnP, o_Aq); // cube threading
    //    scalarEllipticOp(K, o_D, o_vgeo, o_sgeo, o_q, o_qP, o_dqdnM, o_dqdnP, o_Aq);
    //    scalarEllipticOp(K, o_D, o_gz, o_VXYZ, o_sgeo, o_q, o_qP, o_dqdnM, o_dqdnP, o_Aq);
  }

  device.finish();
  double elapsed = occa::toc("scalarEllipticOp");

  o_Aq.copyTo(Aq);
  
  double eflops = 6*Nq*Nq*Nq*Nq;
  eflops += 15*Nq*Nq*Nq;
  eflops += 6*7*Nq*Nq;
  eflops += 2*Nq*Nq*Nq*Nq;
  eflops += 4*Nq*Nq*Nq*Nq;
  eflops += 2*Nq*Nq*Nq;
  eflops += 6*4*5*Nq*Nq;
  
  double gflops = Nit*eflops*K/(1.e9*elapsed);

#if 1
  double ebw = Nq*Nq*Nq*(6+1+1) + Nfaces*Nq*Nq*(1 + 3 + 2 + 2);
#else
  double ebw = 3*8+Nq*Nq*Nq*(1+1) + Nfaces*Nq*Nq*(1 + 3 + 2 + 2);
#endif
  
  double gbw = sizeof(float)*Nit*ebw*K/(1.e9*elapsed);
  
  std::cout << " gflops = " << gflops << std::endl;
  std::cout << " bandwidth = " << gbw << std::endl;

  for(int i = 0; i < 5; ++i)
    std::cout << i << ": " << Aq[i] << '\n';

  std::cout << "GB used: " << device.memoryAllocated()/1.e9 << std::endl;
  
  return 0;
}

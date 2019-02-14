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

#include "adaptive.h"

int pcg(adaptive_t* adaptive,
	dfloat lambda, 
        occa::memory &o_r,
	occa::memory &o_x, 
        const dfloat tol, const int MAXIT){

  
  setupAide options = adaptive->options;

  int fixedIterationCountFlag = 0;
  int enableGatherScatters = 1;
  int enableReductions = 1;
  int flexible = options.compareArgs("KRYLOV SOLVER", "FLEXIBLE");
  int verbose = options.compareArgs("VERBOSE", "TRUE");
  
  options.getArgs("DEBUG ENABLE REDUCTIONS", enableReductions);
  options.getArgs("DEBUG ENABLE OGS", enableGatherScatters);
  if(options.compareArgs("FIXED ITERATION COUNT", "TRUE"))
    fixedIterationCountFlag = 1;
  
  // register scalars
  dfloat rdotz1 = 0;
  dfloat rdotz2 = 0;


  // now initialized
  dfloat alpha = 0, beta = 0, pAp = 0;
  
  /*aux variables */
  occa::memory &o_p  = adaptive->o_p;
  occa::memory &o_z  = adaptive->o_z;
  occa::memory &o_Ap = adaptive->o_Ap;
  occa::memory &o_Ax = adaptive->o_Ax;

  pAp = 0;
  rdotz1 = 1;

  dfloat rdotr0;

  // compute A*x
  adaptiveOperator(adaptive, adaptive->level, lambda, o_x, adaptive->o_Ax, dfloatString);
  
  // subtract r = b - A*x
  adaptiveScaledAdd(adaptive, adaptive->level, -1.f, o_Ax, 1.f, o_r);

  if(enableReductions)
    rdotr0 = adaptiveWeightedNorm2(adaptive, adaptive->level, adaptive->level->ogs->o_invDegree, o_r);
  else
    rdotr0 = 1;

  dfloat TOL =  mymax(tol*tol*rdotr0,tol*tol);
  
  int iter;
  for(iter=1;iter<=MAXIT;++iter){

    // z = Precon^{-1} r
    adaptivePreconditioner(adaptive, lambda, o_r, o_z);

    rdotz2 = rdotz1;

    // r.z
    if(enableReductions){
      rdotz1 = adaptiveWeightedInnerProduct(adaptive, adaptive->o_invDegree, o_r, o_z); 
    }
    else
      rdotz1 = 1;
    
    if(flexible){
      dfloat zdotAp;
      if(enableReductions){
	zdotAp = adaptiveWeightedInnerProduct(adaptive, adaptive->o_invDegree, o_z, o_Ap);  
      }
      else
	zdotAp = 1;
      
      beta = -alpha*zdotAp/rdotz2;
    }
    else{
      beta = (iter==1) ? 0:rdotz1/rdotz2;
    }
    
    // p = z + beta*p
    adaptiveScaledAdd(adaptive, 1.f, o_z, beta, o_p);
    
    // A*p
    adaptiveOperator(adaptive, adaptive->level, lambda, o_p, o_Ap, dfloatString); 

    // dot(p,A*p)
    if(enableReductions){
      pAp =  adaptiveWeightedInnerProduct(adaptive, adaptive->o_invDegree, o_p, o_Ap);
    }
    else
      pAp = 1;

    alpha = rdotz1/pAp;

    //  x <= x + alpha*p
    //  r <= r - alpha*A*p
    //  dot(r,r)
    
    dfloat rdotr = adaptiveUpdatePCG(adaptive, o_p, o_Ap, alpha, o_x, o_r);
	
    if (verbose&&(mesh->rank==0)) {

      if(rdotr<0)
	printf("WARNING CG: rdotr = %17.15lf\n", rdotr);
      
      printf("CG: it %d r norm %12.12le alpha = %le \n", iter, sqrt(rdotr), alpha);    
    }
    
    if(rdotr<=TOL && !fixedIterationCountFlag) break;
    
  }

  return iter;
}


ifndef OCCA_DIR
ERROR:
	@echo "Error, environment variable [OCCA_DIR] is not set"
endif

CXXFLAGS =

include ${OCCA_DIR}/scripts/makefile

# define variables
HDRDIR = ../../include
GSDIR  = ../../3rdParty/gslib.github/src
ALMONDDIR = ../parALMOND

# set options for this machine
# specify which compilers to use for c, fortran and linking
cc	= mpicc.openmpi
CC	= mpic++.openmpi
LD	= mpic++.openmpi

# compiler flags to be used (set to compile with debugging on)
CFLAGS = -I. $(compilerFlags) $(flags) -I$(HDRDIR) -I$(GSDIR) -g  -D DHOLMES='"${CURDIR}/../.."' -D DELLIPTIC='"${CURDIR}"'


# link flags to be used
LDFLAGS	= $(compilerFlags) $(flags) -g -L../../3rdParty/gslib.github  -lgs \
			-L$(ALMONDDIR) -lparALMOND

# libraries to be linked in
LIBS	=  $(links) -L../../3rdParty/BlasLapack -lBlasLapack -lgfortran

#-llapack -lblas

INCLUDES = elliptic.h ellipticPrecon.h
DEPS = $(INCLUDES) \
$(HDRDIR)/mesh.h \
$(HDRDIR)/mesh2D.h \
$(HDRDIR)/mesh3D.h \
$(HDRDIR)/ogs_t.h \
$(ALMONDDIR)/parALMOND.h \

# types of files we are going to construct rules for
.SUFFIXES: .c

# rule for .c files
.c.o: $(DEPS)
	$(CC) $(CFLAGS) -o $*.o -c $*.c $(paths)

# list of objects to be compiled
AOBJS    = \
./src/PCG.o \
./src/ellipticBuildContinuous.o \
./src/ellipticBuildIpdg.o \
./src/ellipticBuildJacobi.o \
./src/ellipticBuildLocalPatches.o \
./src/ellipticBuildMultigridLevel.o \
./src/ellipticHaloExchange.o\
./src/ellipticMultiGridSetup.o \
./src/ellipticOperator.o \
./src/ellipticParallelGatherScatter.o \
./src/ellipticParallelGatherScatterSetup.o \
./src/ellipticPreconditioner.o\
./src/ellipticPreconditionerSetup.o\
./src/ellipticSEMFEMSetup.o\
./src/ellipticSetup.o \
./src/ellipticSmoother.o \
./src/ellipticSmootherSetup.o \
./src/ellipticSolve.o\
./src/ellipticSolveSetup.o\
./src/ellipticVectors.o \

# library objects
LOBJS = \
../../src/meshApplyElementMatrix.o \
../../src/meshConnect.o \
../../src/meshConnectBoundary.o \
../../src/meshConnectFaceNodes2D.o \
../../src/meshConnectFaceNodes3D.o \
../../src/meshGeometricFactorsTet3D.o \
../../src/meshGeometricFactorsHex3D.o \
../../src/meshGeometricFactorsTri2D.o \
../../src/meshGeometricFactorsQuad2D.o \
../../src/meshGeometricPartition2D.o \
../../src/meshGeometricPartition3D.o \
../../src/meshHaloExchange.o \
../../src/meshHaloExtract.o \
../../src/meshHaloSetup.o \
../../src/meshLoadReferenceNodesTri2D.o \
../../src/meshLoadReferenceNodesQuad2D.o \
../../src/meshLoadReferenceNodesTet3D.o \
../../src/meshLoadReferenceNodesHex3D.o \
../../src/meshOccaSetup2D.o \
../../src/meshOccaSetup3D.o \
../../src/meshParallelConnectNodes.o \
../../src/meshParallelConnectOpt.o \
../../src/meshParallelConsecutiveGlobalNumbering.o\
../../src/meshParallelGatherScatter.o \
../../src/meshParallelGatherScatterSetup.o \
../../src/meshParallelReaderTri2D.o \
../../src/meshParallelReaderQuad2D.o \
../../src/meshParallelReaderTet3D.o \
../../src/meshParallelReaderHex3D.o \
../../src/meshPartitionStatistics.o \
../../src/meshPhysicalNodesTri2D.o \
../../src/meshPhysicalNodesQuad2D.o \
../../src/meshPhysicalNodesTet3D.o \
../../src/meshPhysicalNodesHex3D.o \
../../src/meshPlotVTU2D.o \
../../src/meshPlotVTU3D.o \
../../src/meshPrint2D.o \
../../src/meshPrint3D.o \
../../src/meshSetupTri2D.o \
../../src/meshSetupQuad2D.o \
../../src/meshSetupTet3D.o \
../../src/meshSetupHex3D.o \
../../src/meshSurfaceGeometricFactorsTri2D.o \
../../src/meshSurfaceGeometricFactorsQuad2D.o \
../../src/meshSurfaceGeometricFactorsTet3D.o \
../../src/meshSurfaceGeometricFactorsHex3D.o \
../../src/meshVTU2D.o \
../../src/meshVTU3D.o \
../../src/matrixInverse.o \
../../src/matrixConditionNumber.o \
../../src/mysort.o \
../../src/parallelSort.o \
../../src/setupAide.o \
../../src/timer.o\
../../src/readArray.o

COBJS = \
../../src/gsParallelGatherScatter.o\
../../src/gsParallelGatherScatterSetup.o\
../../src/xxtCoarseSolve.o


ellipticMain:$(AOBJS) $(LOBJS) ./src/ellipticMain.o gslibInterface
	cd ../../3rdParty/BlasLapack; make -j lib; cd ../../solvers/elliptic
	cd ../parALMOND; make -j lib; cd ../elliptic
	$(LD)  $(LDFLAGS)  -o ellipticMain ./src/ellipticMain.o $(COBJS) $(AOBJS) $(LOBJS) $(paths) $(LIBS)

lib:$(AOBJS)
	ar -cr libelliptic.a $(AOBJS)

gslibInterface:
	cd ../../3rdParty/gslib.github; make -j libgs.a; cd ../../solvers/elliptic
	$(cc) $(CFLAGS) -c -o ../../src/gsParallelGatherScatter.o ../../src/gsParallelGatherScatter.c $(paths)
	$(cc) $(CFLAGS) -c -o ../../src/gsParallelGatherScatterSetup.o ../../src/gsParallelGatherScatterSetup.c $(paths)
	$(cc) $(CFLAGS) -c -o ../../src/xxtCoarseSolve.o ../../src/xxtCoarseSolve.c $(paths)

all: lib ellipticMain

# what to do if user types "make clean"
clean:
	cd ../parALMOND; make clean; cd ../elliptic
	cd ../../src; rm *.o; cd ../solvers/elliptic
	rm src/*.o ellipticMain libelliptic.a

realclean:
	cd ../../3rdParty/gslib.github; make clean; cd ../../solvers/elliptic
	cd ../../3rdParty/BlasLapack; make clean; cd ../../solvers/elliptic
	cd ../parALMOND; make clean; cd ../elliptic
	cd ../../src; rm *.o; cd ../solvers/elliptic
	rm src/*.o ellipticMain libelliptic.a

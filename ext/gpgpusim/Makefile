GEM5_LIB=gem5_opt
ARCH=X86
OFLAG=3

LDFLAGS=-shared -fno-common -L../../build/${ARCH}/ -Wl,-rpath ../../build/${ARCH}
CXXFLAGS=-std=c++17 -g -O${OFLAG} -fPIC ${shell python3-config --includes} -I../../build/${ARCH}/ -I../../ext/pybind11/include/ -I../../build/softfloat/ -I../../ext -I../../src
CPPFLAGS+=-MMD -MP
SRC=$(wildcard *.cc)

.PHONY: clean all

all: libgem5_to_gpgpusim.so

libgem5_to_gpgpusim.so: $(SRC:%.cc=%.o)
	${CXX} ${CPPFLAGS} ${LDFLAGS} $? -o $@ -l${GEM5_LIB}

-include $(SRC:%.cc=%.d)

clean:
	${RM} *.[do] libgem5_to_gpgpusim.so

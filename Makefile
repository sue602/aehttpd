curr_dir := $(shell pwd)
ifeq ($(curr_dir),/)
$(error curr_dir is $(curr_dir))
endif

all: hiredis 
	cd $(curr_dir)/src; make;

openshift_all: deps
	cd $(curr_dir)/src; make;

deps: hiredis zlib

hiredis:
	cd $(curr_dir)/deps/hiredis; PREFIX=$(curr_dir)/usr make static; PREFIX=$(curr_dir)/usr make install;

zlib:
	cd $(curr_dir)/deps/zlib-1.2.8; ./configure --prefix=$(curr_dir)/usr; make; make install;



debug: deps
	cd $(curr_dir)/src; make debug;

clean:
	rm $(curr_dir)/usr/include/* -rf;
	rm $(curr_dir)/usr/lib/* -rf;
	cd $(curr_dir)/deps/hiredis; make clean;
	cd $(curr_dir)/deps/zlib-1.2.8; make clean;
	cd $(curr_dir)/src; make clean;



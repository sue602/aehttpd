curr_dir := $(shell pwd)
ifeq ($(curr_dir),/)
$(error curr_dir is $(curr_dir))
endif
all:
	cd $(curr_dir)/deps/hiredis; PREFIX=$(curr_dir)/usr make static; PREFIX=$(curr_dir)/usr make install;
	cd $(curr_dir)/src; make;

debug:
	cd $(curr_dir)/src; make debug;

clean:
	rm $(curr_dir)/usr/include/* -rf;
	rm $(curr_dir)/usr/lib/* -rf;
	cd $(curr_dir)/deps/hiredis; make clean;
	cd $(curr_dir)/src; make clean;



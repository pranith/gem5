all: test_arm_lsq

test_arm_lsq: test_arm_lsq.c
	aarch64-linux-gnu-gcc --static -o $@ $<

clean:
	rm -f test_arm_lsq

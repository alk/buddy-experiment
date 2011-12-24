
CFLAGS := -O3 -march=i686 -ggdb3 -Wall -std=gnu99

buddy-experiment: buddy-experiment.o sha1.o

#	gcc $LDFLAGS -o $@ $<

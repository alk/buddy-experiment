
CFLAGS := -O3 -march=native -ggdb3 -Wall -std=gnu99

buddy-experiment: buddy-experiment.o sha1.o

clean:
	rm -f buddy-experiment buddy-experiment.o sha1.o

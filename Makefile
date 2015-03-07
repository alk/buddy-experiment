
CFLAGS := -O3 -march=native -ggdb3 -Wall -std=gnu99

OBJS := main.o buddy-experiment.o jemalloc-adaptor.o mini-adaptor.o dl-adaptor.o chunky-generic.o dl-malloc.o

all: buddy-experiment

buddy-experiment: $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $^

dl-malloc.o: CPPFLAGS := -DUSE_DL_PREFIX

# $(OBJS): common.h

# buddy-experiment-jm: main.o jemalloc-adaptor.o
# 	$(CC) -o $@ $(LDFLAGS) $^ -ljemalloc

# buddy-experiment-jmc: main.o chunky-je.o
# 	$(CC) -o $@ $(LDFLAGS) $^ -ljemalloc

# buddy-experiment-mini: main.o chunky-mini.o minimalloc.o
# 	$(CC) -o $@ $(LDFLAGS) $^

clean:
	rm -f buddy-experiment $(OBJS) jemalloc-adaptor.o chunky-je.o

CC = gcc
CFLAG = -std=gnu11

OBJS := xs.o
deps := $(OBJS:%.o=.%.o.d)

xs: $(OBJS)
	$(CC) -o $@ $^ 

%.o: %.c
	$(CC) $(CFLAG) -o $@ -c -MMD -MF .$@.d $^

clean:
	rm -f $(OBJS) $(deps) xs
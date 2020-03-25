all: clean
	gcc -Wall -g master.c -o master -lm -lrt -lpthread
	gcc -Wall -g bin_adder.c -o bin_adder -lrt

clean:
	rm -rf master bin_adder *.log

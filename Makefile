
INCLUDE = /usr/local/include
LIBS = -lpthread -lboost_program_options
LIBPATH = /usr/local/lib
OBJS = main.o 

.cc.o:
	c++ -I$(INCLUDE) -c $<

NXCore_icom: $(OBJS)
	c++ -L$(LIBPATH) $(LIBS) -o NXCore_icom $(OBJS) 

clean:
	rm -f *.o NXCore_icom

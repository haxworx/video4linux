FLAGS=`pkg-config --libs --cflags evas ecore-evas elementary`

default:
	$(CC) -O0 -g -ggdb3 cam.c $(FLAGS) -o cam
	$(CC) -O0 -g -ggdb3 vid.c $(FLAGS) -o vid
clean:
	-rm cam
	-rm vid

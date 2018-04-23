all: metro.c
	gcc -o metro metro.c -lncursesw -lpthread -finput-charset=UTF-8

clearlogs:
	find . -name "*.log" -type f -delete
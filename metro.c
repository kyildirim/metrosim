#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <locale.h>
#include <time.h>
#include <signal.h>

#include <pthread.h>
#include <ncurses.h>
#include <menu.h>
#include <argp.h>

//Program definitions
#define PROGRAM_VERSION "v1.0b"

//Error definitions
#define MIN_SIZE_MIS -11
#define ARGP_ERR_UNKNOWN -21
#define INV_MENU_OPT -31
#define INV_SETT_OPT_VAL -32

//Window definitions
#define COLS_MIN 80
#define LINES_MIN 24
#define METRO_LINES 15 
#define METRO_COLS 49
#define SPLASH_TIME 1

//Color definitions
#define WHITE_BLACK 0
#define GREEN_BLACK 1
#define RED_BLACK 2
#define YELLOW_BLACK 3
#define MARKED_TEXT A_STANDOUT

//Menu definitions
#define NUM_MENU_OPTS 5
#define MENU_START 0
#define MENU_SETTINGS 1
#define MENU_LOGS 2
#define MENU_HELP 3
#define MENU_EXIT 4

#define NUM_SETT_OPTS 3
#define SETT_TIME 0
#define SETT_PROB 1
#define SETT_DONE 2

//Simulation definitions
#define SIM_TIME_MAX 9999

//Argp vars
const char *argp_program_version = "MetroSim v0.1b";
const char *argp_program_bug_address = "<kyildirim14@ku.edu.tr>";
static char doc[] = "A simple metro simulation using pthreads to simulate lines.";

enum optioncodes{ 
	OPT_TIME = 's',
	OPT_PROB = 'p'
};

static char args_doc[] = "TO-DO Implement";

static struct argp_option options[] =
{
	{"time", OPT_TIME, "TIME", 0, "Simulation time in seconds."},
	{"probability", OPT_PROB, "PROB", 0, "Probability of a train arriving in unit time."}
};

//Global windows
WINDOW *metro_container;
WINDOW *console_container;
WINDOW *metro_window;
WINDOW *console_window;

//Console vars
int console_max_lines = NULL;
char **console_lines = NULL;
int *console_line_color = NULL;
int console_line_counter = 0;

//Train struct
struct Train{
	int id;
	int length;
	int arrival_time;
	int departure_time;
	char origin;
	char destination;
	int broken;
};

//Train vars
int train_counter = 1;
int releasing_segment_id = -1;
int *queue_status = NULL;
pthread_mutex_t train_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t probability_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
int can_release = 1;
int tunnel_ticks = 0;

//Simulation vars
float probability = 0.5f;
int simulation_time = 10;
int queue_count = 4;
int tick = 0;
time_t raw_time = NULL;
struct tm *time_data = NULL;
pthread_barrier_t tick_barrier;
pthread_barrier_t main_barrier;
pthread_t threads[4];
pthread_mutex_t queue_count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t tunnel_tick_mutex = PTHREAD_MUTEX_INITIALIZER;
struct Train queue_leaders[4];
pthread_mutex_t queue_leader_mutex = PTHREAD_MUTEX_INITIALIZER;
struct Train train_in_tunnel;
int total_trains = 0;
int allow_trains = 1;

//Map vars
int *segment_colors = NULL;
int tunnel_color = 1;
char segment_names[4] = {'A', 'B', 'E', 'F'};

//Logging vars
FILE *train_log;
FILE *control_log;
pthread_mutex_t log_train_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_control(const char *format, ...){
	va_list args;
	va_start(args, format);
	vfprintf(control_log, format, args);
	va_end(args);
}

void log_train(const char *format, ...){
	pthread_mutex_lock(&log_train_mutex);
	va_list args;
	va_start(args, format);
	vfprintf(train_log, format, args);
	va_end(args);
	pthread_mutex_unlock(&log_train_mutex);
}

int get_train_id(){
	int id = -1;
	pthread_mutex_lock(&train_counter_mutex);
	id = train_counter;
	train_counter++;
	pthread_mutex_unlock(&train_counter_mutex);
	return id;
}

int get_probability(float p){
	int r = 0;
	pthread_mutex_lock(&probability_mutex);
	float random = (float)rand() / (float)RAND_MAX;
	if(random<=p)r=1;
	pthread_mutex_unlock(&probability_mutex);
	return r;
}

void recolor_lanes(){
	int min = simulation_time;
	int max = 0;
	for(int i = 0; i<queue_count; i++){
		if(queue_status[i]>max)max=queue_status[i];
		if(queue_status[i]<min)min=queue_status[i];
	}
	for(int i = 0; i<queue_count; i++){
		if(queue_status[i]==max){
			segment_colors[i]=RED_BLACK;
		}else if(queue_status[i]==min){
			segment_colors[i]=GREEN_BLACK;
		}else{
			segment_colors[i]=YELLOW_BLACK;
		}
		if(min==max)segment_colors[i]=YELLOW_BLACK;
		if(max==0)segment_colors[i]=GREEN_BLACK;
	}
}

void decide_releasing_queue(){
	if(can_release!=1)return;
	pthread_mutex_lock(&queue_count_mutex);
	int max_queue=-1;
	int max_count=0;
	for(int i = 0; i<queue_count; i++){
		if(queue_status[i]>max_count){
			max_queue=i;
			max_count=queue_status[i];
		}
	}
	releasing_segment_id = max_queue;
	pthread_mutex_unlock(&queue_count_mutex);
}

void update_tunnel_tick(int update){
	pthread_mutex_lock(&tunnel_tick_mutex);
	tunnel_ticks+=update;
	if(tunnel_ticks<=0){
		can_release=1;
		train_in_tunnel.id=NULL;
	}
	pthread_mutex_unlock(&tunnel_tick_mutex);
}

int get_tunnel_ticks(){
	pthread_mutex_lock(&tunnel_tick_mutex);
	int rvalue = tunnel_ticks;
	pthread_mutex_unlock(&tunnel_tick_mutex);
	return rvalue;
}

void update_queue_leader(int segment_id, struct Train t){
	pthread_mutex_lock(&queue_leader_mutex);
	queue_leaders[segment_id] = t;
	pthread_mutex_unlock(&queue_leader_mutex);
}

void update_queues(int segment_id, int queued_count){
	pthread_mutex_lock(&queue_count_mutex);
	queue_status[segment_id] = queued_count;
	pthread_mutex_unlock(&queue_count_mutex);
}

int count_trains(){
	int count = 0;
	for(int i = 0; i<queue_count; i++){
		count+=queue_status[i];
	}
	return count;
}

void *segment_handler(int segment_id){

	//Initialize segment
	struct Train *queue = malloc(simulation_time*sizeof(struct Train));
	int queue_counter = 0;
	float p = probability;
	//Exception for B
	if(segment_id==1)p=1-p;

	for(;;){
		if(releasing_segment_id==segment_id){
			struct Train t;
			t=queue[0];
			char message[COLS-2];
			snprintf(message, COLS-2, "[SEGMENT %c] Released train with ID %04d.", segment_names[segment_id], t.id);
			log_console(GREEN_BLACK, message);
			queue_counter--;
			train_in_tunnel = t;
			printf("%d", t.length+1+(4*t.broken));
			update_tunnel_tick(t.length+1+1+(4*t.broken));
			can_release=(t.broken==1)?RED_BLACK:YELLOW_BLACK;
			memmove(&queue[0], &queue[1], queue_counter*sizeof(struct Train));
		}
		if(1==get_probability(p)&&allow_trains==1){
			struct Train t;
			t.id = get_train_id();
			t.origin = segment_names[segment_id];
			t.length = 1+get_probability(0.3f);
			t.broken = get_probability(0.1f);
			t.destination = segment_names[(segment_id/2+(2+get_probability(0.5f)))%4];
			t.arrival_time = tick;
			queue[queue_counter]=t;
			queue_counter++;
		}
		update_queues(segment_id, queue_counter);
		update_queue_leader(segment_id, queue[0]);//Optimize this
		char message[COLS-2];
		snprintf(message, COLS-2, "[SEGMENT %c] %d trains in queue.", segment_names[segment_id], queue_counter);
		log_console(GREEN_BLACK, message);
		pthread_barrier_wait(&tick_barrier);
		pthread_barrier_wait(&main_barrier);
	}

}

void log_console(int color, char *message){
	pthread_mutex_lock(&log_mutex);
	time(&raw_time);
	time_data = localtime(&raw_time);
	char line[COLS-2];
	snprintf(line, COLS-2, "[%02d:%02d:%02d]%s",time_data->tm_hour,time_data->tm_min,time_data->tm_sec,message);
	message = line;
	if(console_line_counter<console_max_lines){
		memcpy(console_lines[console_line_counter], message, COLS-2);
		//console_lines[console_line_counter]=message;
		memcpy(&console_line_color[console_line_counter], &color, sizeof(int));
		//console_line_color[console_line_counter]=color;
		console_line_counter++;
	}else{
		int i = 0;
		for(i; i<console_max_lines-1; i++){
			memcpy(console_lines[i], console_lines[i+1], COLS-2);
			//console_lines[i]=console_lines[i+1];
			memcpy(&console_line_color[i], &console_line_color[i+1], sizeof(int));
			//console_line_color[i]=console_line_color[i+1];
			//memmove(&console_line_color[0], &console_line_color[1], (console_max_lines-1)*sizeof(int));
		}
		memcpy(console_lines[console_max_lines-1], message, COLS-2);
		//console_lines[console_max_lines]=message;
		memcpy(&console_line_color[console_max_lines-1], &color, sizeof(int));
		//console_line_color[console_max_lines]=color;
	}
	pthread_mutex_unlock(&log_mutex);
}

void print_time(){
	time(&raw_time);
	time_data = localtime(&raw_time);
	wmove(metro_container, METRO_LINES+1,2);
	wprintw(metro_container, "Current time: %02d:%02d:%02d Tick: %d",time_data->tm_hour,time_data->tm_min,time_data->tm_sec,tick);
	wrefresh(metro_container);
}

void print_console(){
	wclear(console_window);
	int i = 0;
	for(i;i<console_max_lines;i++){
		wmove(console_window,i,0);
		wattron(console_window, COLOR_PAIR(console_line_color[i]));
		wprintw(console_window, "%s", console_lines[i]);
	}
	wrefresh(console_window);
}

void draw_map(int *colors){

	int i;

	//Segment 1
	wattron(metro_window, COLOR_PAIR(colors[0]));
	wmove(metro_window, 0,1);
	if(queue_leaders[0].id!=NULL){
		wprintw(metro_window, "T(%04d)", queue_leaders[0].id);
	}else{
		wprintw(metro_window, "       ");//Replace with clear
	}
	wmove(metro_window, 1,0);
	if(releasing_segment_id==0)wattron(metro_window, MARKED_TEXT);
	wprintw(metro_window, "A══════════╗");
	for(i=0; i<5; i++){
		wmove(metro_window, 2+i,11+i);
		wprintw(metro_window, "╚╗");
	}
	wattroff(metro_window, MARKED_TEXT);
	wmove(metro_window, 5,0);
	wprintw(metro_window, "%d trains", queue_status[0]);
	wmove(metro_window, 6,0);
	wprintw(metro_window, "in queue");

	//Segment 2
	wattron(metro_window, COLOR_PAIR(colors[1]));
	wmove(metro_window, 12,1);
	if(queue_leaders[1].id!=NULL){
		wprintw(metro_window, "T(%04d)", queue_leaders[1].id);
	}else{
		wprintw(metro_window, "       ");//Replace with clear
	}
	wmove(metro_window, 13,0);
	if(releasing_segment_id==1)wattron(metro_window, MARKED_TEXT);
	wprintw(metro_window, "B══════════╝");
	for(i=0; i<5; i++){
		wmove(metro_window, 12-i,11+i);
		wprintw(metro_window, "╔╝");
	}
	wattroff(metro_window, MARKED_TEXT);
	wmove(metro_window, 8,0);
	wprintw(metro_window, "%d trains", queue_status[1]);
	wmove(metro_window, 9,0);
	wprintw(metro_window, "in queue");

	//Segment 3
	wattron(metro_window, COLOR_PAIR(colors[2]));
	wmove(metro_window, 0,40);
	if(queue_leaders[2].id!=NULL){
		wprintw(metro_window, "T(%04d)", queue_leaders[2].id);
	}else{
		wprintw(metro_window, "       ");//Replace with clear
	}
	wmove(metro_window, 1,36);
	if(releasing_segment_id==2)wattron(metro_window, MARKED_TEXT);
	wprintw(metro_window, "╔══════════E");
	for(i=0; i<5; i++){
		wmove(metro_window, 2+i,35-i);
		wprintw(metro_window, "╔╝");
	}
	wattroff(metro_window, MARKED_TEXT);
	wmove(metro_window, 5,37);
	wprintw(metro_window, "%d trains", queue_status[2]);
	wmove(metro_window, 6,37);
	wprintw(metro_window, "in queue");

	//Segment 4
	wattron(metro_window, COLOR_PAIR(colors[3]));
	wmove(metro_window, 12,40);
	if(queue_leaders[3].id!=NULL){
		wprintw(metro_window, "T(%04d)", queue_leaders[3].id);
	}else{
		wprintw(metro_window, "       ");//Replace with clear
	}
	wmove(metro_window, 13,36);
	if(releasing_segment_id==3)wattron(metro_window, MARKED_TEXT);
	wprintw(metro_window, "╚══════════F");
	for(i=0; i<5; i++){
		wmove(metro_window, 12-i,35-i);
		wprintw(metro_window, "╚╗");
	}
	wattroff(metro_window, MARKED_TEXT);
	wmove(metro_window, 8,37);
	wprintw(metro_window, "%d trains", queue_status[3]);
	wmove(metro_window, 9,37);
	wprintw(metro_window, "in queue");

	//Tunnel
	wattron(metro_window, COLOR_PAIR(can_release));
	if(train_in_tunnel.id!=NULL){
		if(train_in_tunnel.origin-'A'<2){
			wmove(metro_window, 6, 19);
			wprintw(metro_window, "T(%04d)->%c",train_in_tunnel.id, train_in_tunnel.destination);
		}else{
			wmove(metro_window, 8, 19);
			wprintw(metro_window, "%c<-T(%04d)", train_in_tunnel.destination, train_in_tunnel.id);
		}
	}else{
		//Replace with clear
		wmove(metro_window, 6, 19);
		wprintw(metro_window, "          ");
		wmove(metro_window, 8, 19);
		wprintw(metro_window, "          ");
	}
	wmove(metro_window, 7,16);
	wprintw(metro_window, "╠═C━━━━━━━━━━D═╣");
	
	wrefresh(metro_window);
}

void update_metro_container(int color){
	//Metro container
	metro_container = newwin(METRO_LINES+2,COLS,0,0);
	wattron(metro_container, COLOR_PAIR(color));
	wborder(metro_container, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
	wmove(metro_container, 0,2);
	wprintw(metro_container, "Metro Map");
	wrefresh(metro_container);
	draw_map(segment_colors);
	print_time();
}

int ncurses_init_windows(){

	clear();
	refresh();
	//Metro container
	metro_container = newwin(METRO_LINES+2,COLS,0,0);
	wattron(metro_container, COLOR_PAIR(GREEN_BLACK));
	wborder(metro_container, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
	wmove(metro_container, 0,2);
	wprintw(metro_container, "Metro Map");
	wrefresh(metro_container);

	//Calculate maximum lines
	console_max_lines = LINES-METRO_LINES-2-2;
	console_lines = malloc(sizeof(char *)*console_max_lines);
	int i = 0;
	for(i;i<console_max_lines;i++){
		console_lines[i]=malloc(sizeof(char)*(COLS-2));
		strcpy(console_lines[i], "");
	}
	console_line_color = malloc(sizeof(int)*console_max_lines);

	//Console container
	console_container = newwin(console_max_lines+2, COLS, METRO_LINES+2, 0);
	wattron(console_container, COLOR_PAIR(GREEN_BLACK));
	wborder(console_container, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
	wmove(console_container, 0,2);
	wprintw(console_container, "Console");
	wrefresh(console_container);

	//Metro window
	metro_window = newwin(METRO_LINES,METRO_COLS,1,(COLS-METRO_COLS)/2);
	wrefresh(metro_window);

	//Console window
	console_window = newwin(console_max_lines, COLS-2, METRO_LINES+3, 1);
	wrefresh(console_window);


	refresh();
}

int ncurses_init(){
	//ncurses init
	setlocale(LC_CTYPE, "");
	initscr();
	cbreak();
	noecho();
	curs_set(0);

	//Terminal size not sufficient
	if(COLS<COLS_MIN||LINES<LINES_MIN)return MIN_SIZE_MIS;

	//ncurses color init
	start_color();
	init_pair(WHITE_BLACK, COLOR_WHITE, COLOR_BLACK);
	init_pair(GREEN_BLACK, COLOR_GREEN, COLOR_BLACK);
	init_pair(RED_BLACK, COLOR_RED, COLOR_BLACK);
	init_pair(YELLOW_BLACK, COLOR_YELLOW, COLOR_BLACK);


	refresh();
}

void sigwinch_handler(int signal){
	//Init ncurses windows
	ncurses_init_windows();
}

int get_central_start(char *str){
	return (COLS-strlen(str))/2;
}

void init_settings_menu(){

	//formats redundant now
	WINDOW *settings_menu = newwin(LINES, COLS, 0, 0);
	keypad(settings_menu, TRUE);
	wattron(settings_menu, COLOR_PAIR(GREEN_BLACK));
	wborder(settings_menu, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
	int selected_option = 0;
	int col = 0;
	int item_start = COLS/4;
	int value_start = 3*COLS/4;
	const char *menu_header = "Settings";
	const char *menu_options[NUM_SETT_OPTS];
	const char *menu_option_formats[NUM_SETT_OPTS];
	menu_options[SETT_TIME] = "Simulation Time:";
	menu_option_formats[SETT_TIME] = "%d seconds";
	menu_options[SETT_PROB] = "Probability:";
	menu_option_formats[SETT_PROB] = "%.3f";
	menu_options[SETT_DONE] = "Done";
	void *test = 1;
	char key;
	for(;;){
		int line_start = (LINES-NUM_SETT_OPTS-2)/2;
		wmove(settings_menu, line_start, get_central_start(menu_header));
		wattron(settings_menu, A_UNDERLINE);
		wprintw(settings_menu, menu_header);
		wattroff(settings_menu, A_UNDERLINE);
		line_start+=2;
		for(int i = 0; i<NUM_SETT_OPTS; i++){
			if(i==NUM_SETT_OPTS-1){
				wmove(settings_menu, line_start+i, get_central_start(menu_options[i]));
				if(i==selected_option&&col==0)wattron(settings_menu, MARKED_TEXT);
				wprintw(settings_menu, "%s", menu_options[i]);
				if(i==selected_option&&col==0)wattroff(settings_menu, MARKED_TEXT);
				continue;
			}
			wmove(settings_menu, line_start+i, item_start);
			if(i==selected_option&&col==0)wattron(settings_menu, MARKED_TEXT);
			wprintw(settings_menu, "%s", menu_options[i]);
			if(i==selected_option&&col==0)wattroff(settings_menu, MARKED_TEXT);
			wmove(settings_menu, line_start+i, value_start);
			if(i==selected_option&&col==1)wattron(settings_menu, MARKED_TEXT);
			int cX,cY;
			switch(i){
				case SETT_TIME:
					getyx(settings_menu, cY, cX);
					if(i==selected_option&&col==1)wattroff(settings_menu, MARKED_TEXT);
					wprintw(settings_menu, "            ");//Not good practice
					if(i==selected_option&&col==1)wattron(settings_menu, MARKED_TEXT);
					wmove(settings_menu, cY, cX);
					wprintw(settings_menu, menu_option_formats[i], simulation_time);
					break;
				case SETT_PROB:
					wprintw(settings_menu, menu_option_formats[i], probability);
					break;

			}
			if(i==selected_option&&col==1)wattroff(settings_menu, MARKED_TEXT);
		}
		wrefresh(settings_menu);
		int key = wgetch(settings_menu);
		if(key==10)col=(col==0)?1:0;
		if(selected_option==SETT_DONE&&col==1)return;
		if(key==KEY_UP&&col==0)selected_option--;
		if(key==KEY_DOWN&&col==0)selected_option++;
		if(selected_option>NUM_SETT_OPTS-1)selected_option%=NUM_SETT_OPTS;
		if(selected_option<0)selected_option+=NUM_SETT_OPTS;
		if(col==1){
			switch(selected_option){
				case SETT_TIME:
					if(key==KEY_UP&&simulation_time<=SIM_TIME_MAX)simulation_time++;
					if(key==KEY_DOWN&&simulation_time>1)simulation_time--;
					if(key>'0'-1&&key<'9'+1){
						simulation_time*=10;
						simulation_time+=key-'0';
						if(simulation_time>SIM_TIME_MAX)simulation_time=SIM_TIME_MAX;
					}
					if(key==127||key==8||key==263)simulation_time/=10;
					break;
				case SETT_PROB:
					if(key==KEY_UP&&probability<1.0f)probability+=0.001f;
					if(key==KEY_DOWN&&probability>0.0f)probability-=0.001f;
					if(key>'0'-1&&key<'9'+1){
						probability*=10.0f;
						probability+=(key-'0')*0.001f;
						if(probability>1.0f)probability=1.0f;
					}
					if(key==127||key==8||key==263)probability/=10.0f;
					if(probability<=0.0009f)probability=0.0f;
			}
		}
	}
	wclear(settings_menu);
	werase(settings_menu);
	return;

}

int init_menu_screen(){

	WINDOW *menu_screen = newwin(LINES, COLS, 0, 0);
	keypad(menu_screen, TRUE);
	wattron(menu_screen, COLOR_PAIR(GREEN_BLACK));
	wborder(menu_screen, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
	int selected_option = 0;
	const char *menu_header = "Main Menu";
	const char *menu_options[NUM_MENU_OPTS];
	menu_options[MENU_START] = "Start";
	menu_options[MENU_SETTINGS] = "Settings";
	menu_options[MENU_LOGS] = "Log Viewer";
	menu_options[MENU_HELP] = "Help";
	menu_options[MENU_EXIT] = "Exit";
	char key;
	for(;;){
		int line_start = (LINES-NUM_MENU_OPTS-2)/2;
		wmove(menu_screen, line_start, get_central_start(menu_header));
		wattron(menu_screen, A_UNDERLINE);
		wprintw(menu_screen, menu_header);
		wattroff(menu_screen, A_UNDERLINE);
		line_start+=2;
		for(int i = 0; i<NUM_MENU_OPTS; i++){
			wmove(menu_screen, line_start+i, get_central_start(menu_options[i]));
			if(i==selected_option)wattron(menu_screen, MARKED_TEXT);
			wprintw(menu_screen, "%s", menu_options[i]);
			if(i==selected_option)wattroff(menu_screen, MARKED_TEXT);
		}
		wrefresh(menu_screen);
		int key = wgetch(menu_screen);
		if(key==10)break;
		if(key==KEY_UP)selected_option--;
		if(key==KEY_DOWN)selected_option++;
		if(selected_option>NUM_MENU_OPTS-1)selected_option%=NUM_MENU_OPTS;
		if(selected_option<0)selected_option+=NUM_MENU_OPTS;
	}
	wclear(menu_screen);
	werase(menu_screen);
	return selected_option;
}

void init_splash_screen(){
	WINDOW *splash_screen = newwin(LINES, COLS, 0, 0);
	wattron(splash_screen, COLOR_PAIR(GREEN_BLACK));
	wborder(splash_screen, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
	int x = (LINES-12)/2;
	int y = (COLS-53)/2;
	wmove(splash_screen, x, y);
	wprintw(splash_screen, " __  __        _                _____  _            ");
	wmove(splash_screen, x+1, y);
	wprintw(splash_screen, "|  \\/  |      | |              / ____|(_)           ");
	wmove(splash_screen, x+2, y);
	wprintw(splash_screen, "| \\  / |  ___ | |_  _ __  ___ | (___   _  _ __ ___  ");
	wmove(splash_screen, x+3, y);
	wprintw(splash_screen, "| |\\/| | / _ \\| __|| '__|/ _ \\ \\___ \\ | || '_ ` _ \\ ");
	wmove(splash_screen, x+4, y);
	wprintw(splash_screen, "| |  | ||  __/| |_ | |  | (_) |____) || || | | | | |");
	wmove(splash_screen, x+5, y);
	wprintw(splash_screen, "|_|  |_| \\___| \\__||_|   \\___/|_____/ |_||_| |_| |_|");
	wmove(splash_screen, x+6, y);
	wprintw(splash_screen, "                    __     ___   _                  ");
	wmove(splash_screen, x+7, y);
	wprintw(splash_screen, "                   /_ |   / _ \\ | |                 ");
	wmove(splash_screen, x+8, y);
	wprintw(splash_screen, "             __   __| |  | | | || |__               ");
	wmove(splash_screen, x+9, y);
	wprintw(splash_screen, "             \\ \\ / /| |  | | | || '_ \\              ");
	wmove(splash_screen, x+10, y);
	wprintw(splash_screen, "              \\ V / | | _| |_| || |_) |             ");
	wmove(splash_screen, x+11, y);
	wprintw(splash_screen, "               \\_/  |_|(_)\\___/ |_.__/              ");
	wrefresh(splash_screen);
	sleep(SPLASH_TIME);
	werase(splash_screen);
}

struct arguments {
	float p;
	int s;
};

static error_t parse_opt (int key, char *arg, struct argp_state *state){

	struct arguments *args = state->input;

	switch(key){
		case 'p':
			probability = atof(arg);
			break;
		case 's':
			simulation_time = atoi(arg);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc};

int main(int argc, char **argv){

	struct arguments args;
	args.p=probability;
	args.s=simulation_time;
	argp_parse(&argp, argc, argv, 0, 0, &args);
	//probability=args.p;
	//simulation_time=args.s;

	//Start&Config ncurses
	int ncurses_status = ncurses_init();
	//Handle init errors for ncurses
	if(ncurses_status<0){
		//Print error code
		printf("Ncurses: Error %d\n", ncurses_status);
		//Print error description
		if(ncurses_status == MIN_SIZE_MIS)printf("Minimum size mismatch, this program requires a terminal that is at least %dx%d\n", COLS_MIN,LINES_MIN);
		//Return with error code
		return ncurses_status;
	}

	//Splash screen
	init_splash_screen();

	int menu_option = init_menu_screen();
	//Menu
	while(menu_option!=MENU_START){
		switch(menu_option){
			case MENU_START:
				break;
			case MENU_SETTINGS:
				clear();
				refresh();
				init_settings_menu();
				//init settings menu
				break;
			case MENU_LOGS:
				//switch to log viewer
				break;
			case MENU_HELP:
				//show help
				break;
			case MENU_EXIT:
				clear();
				refresh();
				exit(0);
			default:
				exit(INV_MENU_OPT);
		}
		menu_option=init_menu_screen();
	}

	//Open files
	time(&raw_time);
	time_data = localtime(&raw_time);
	char train_log_file[19];
	snprintf(train_log_file, 19, "%02d:%02d:%02d-train.log", time_data->tm_hour, time_data->tm_min, time_data->tm_sec);
	char control_log_file[21];
	snprintf(control_log_file, 21, "%02d:%02d:%02d-control.log", time_data->tm_hour, time_data->tm_min, time_data->tm_sec);
	train_log = fopen(train_log_file, "w");
	control_log = fopen(control_log_file, "w");

	//Init ncurses windows
	ncurses_init_windows();

	signal(SIGWINCH, sigwinch_handler);

	//Init train queues
	queue_status = calloc(queue_count, sizeof(int));
	segment_colors = malloc(queue_count*sizeof(int));

	//Initial colors
	for (int i = 0; i < queue_count; i++)segment_colors[i]=1;

	draw_map(segment_colors);

	pthread_barrier_init(&tick_barrier, NULL, 5);
	pthread_barrier_init(&main_barrier, NULL, 5);
	for(int i = 0; i<4; i++){
		pthread_create(&threads[i], NULL, segment_handler, i);
	}

	log_control("[%02d:%02d:%02d][CONTROL] Starting simulation with s=%d p=%f\n", time_data->tm_hour, time_data->tm_min, time_data->tm_sec, simulation_time, probability);

	for(;;){
		pthread_barrier_wait(&tick_barrier);
		int num_trains = count_trains();
		if(num_trains>=10&&allow_trains==1){
			allow_trains=0;
			update_metro_container(RED_BLACK);
			log_control("[%02d:%02d:%02d][CONTROL] Blocking incoming trains as total number of trains reached %d.\n", time_data->tm_hour, time_data->tm_min, time_data->tm_sec, num_trains);
		}
		if(num_trains==0){
			allow_trains=1;
			update_metro_container(GREEN_BLACK);
			log_control("[%02d:%02d:%02d][CONTROL] Allowing incoming trains as total number of trains reached %d.\n", time_data->tm_hour, time_data->tm_min, time_data->tm_sec, num_trains);
		}
		releasing_segment_id = -1;
		decide_releasing_queue();
		char line[COLS-2];
		if(releasing_segment_id!=-1){
			log_control("[%02d:%02d:%02d][CONTROL] Signalling segment %c to release train with ID %04d.\n", time_data->tm_hour, time_data->tm_min, time_data->tm_sec, segment_names[releasing_segment_id], queue_leaders[releasing_segment_id].id);
			snprintf(line, COLS-2, "[CONTROL] Signalling segment %c to release train with ID %04d.", segment_names[releasing_segment_id], queue_leaders[releasing_segment_id].id);
		}else{
			log_control("[%02d:%02d:%02d][CONTROL] Cannot release train, tunnel is busy.\n", time_data->tm_hour, time_data->tm_min, time_data->tm_sec);
			snprintf(line, COLS-2, "[CONTROL] Cannot release train, tunnel is busy.");
		}
		log_console(can_release, line);
		recolor_lanes();
		sleep(1);
		print_console();
		draw_map(segment_colors);
		if(tick==simulation_time)break;
		tick++;
		print_time();
		update_tunnel_tick(-1);
		pthread_barrier_wait(&main_barrier);	
	}
	log_control("[%02d:%02d:%02d][CONTROL] Simulation successfully ended.\n", time_data->tm_hour, time_data->tm_min, time_data->tm_sec);

	//Close files
	fclose(control_log);
	fclose(train_log);

	//Debug stop
	wmove(metro_container, METRO_LINES+1, COLS-2-17);
	wprintw(metro_container, "End of Simulation");
	wrefresh(metro_container);
	getch();

	//Stop ncurses
	endwin();

	return 0;

}
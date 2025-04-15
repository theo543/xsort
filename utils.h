void pipe_(int *pipefds);
void close_(int fd);
void write_(int fd, char *buf, int bytes);
void read_(int fd, char *buf, int bytes);
void write_int(int fd, int data);
int read_int(int fd);
int i_min(int a, int b);
int i_max(int a, int b);

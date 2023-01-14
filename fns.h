/* er.c */
void    append(Array*, short, ...);
size_t  bufaddr(size_t);
int     digits(long);
size_t  len(void);
void    resize(Array*, short);
ssize_t writeall(int, const char*, size_t);
ssize_t writef(int);

/* ui.c */
void bar(const char*, ...);
int  dialogue(const char*);
int  dims(void);
void display(void);
int  key(void);
int  rows(void);
void terminit(void);
void termreset(void);

/* util.c */
void err(int);
int  next(size_t*);
void save(void);
void siginit(void);
void sigpend(void);

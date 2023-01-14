/* er.c */
void    append(Array*, short, ...);
size_t  bufaddr(size_t);
void    delete(size_t, int);
int     digits(long);
void    insert(size_t, char, int);
size_t  len(void);
void    move(size_t);
void    record(short, size_t, char);
void    resize(Array*, short);
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
void    err(int);
int     fileopen(const char*, size_t*);
int     next(size_t*);
void    save(void);
void    siginit(void);
void    sigpend(void);
void    search(size_t*, size_t*, int, int);
ssize_t writeall(int, const char*, size_t);

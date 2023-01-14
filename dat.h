#define LEN(X)  (sizeof(X) / sizeof(X[0]))
#define invalid "�"

enum
{
	Gaplen  = 256, /* number of bytes in a full gap */
};

/* error handling status */
enum
{
	Ok,
	Panic, /* Unrecoverable errors */
	Reset  /* Recoverable errors */
};

/* editing mode */
enum
{
	Command,
	Input,
	Select
};

/* dynamic array operation type */
enum
{
	Char,
	Chnge
};

/* undo stack type */
enum
{
	Udelete,
	Uinsert,
	Uend     /* sentinel for sequence of changes */
};

typedef struct Change Change;
typedef struct Array Array;
typedef struct Buffer Buffer;

/* textual change */
struct Change
{
	short  type; /* undo stack type */
	size_t i;    /* byte offset of change */
	char   c;    /* value of change */
};

/* dynamic array */
struct Array
{
	void   *data; /* contents */
	size_t len;   /* length */
	size_t cap;   /* capacity */
};

/* editing buffer */
struct Buffer
{
	char   *c;                  /* contents */
	char   path[PATH_MAX];      /* filename */
	Array  changes;             /* undo stack */
	short  dirty;               /* modified flag */
	size_t *lead, addr1, addr2; /* selection offsets */
	size_t cap, gap, start;     /* gap book-keeping */
	size_t vstart, vline;       /* display book-keeping */
};

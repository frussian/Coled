/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

#define COLED_VERSION "0.0.1"
#define COLED_TAB_STOP 8
#define COLED_QUIT_TIMES 3
#define MAXPASSLEN 32
#define IDLEN 20

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/
typedef struct netConfig {
  char *serverIp;
  int serverPort, server;
  char connected;
  char *id, *pass;
  time_t connectInterval;
} netConfig;

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  struct termios orig_termios;
  int screenrows, screencols, screenHeight, screenWidth;
  int cx, cy;
  int rx;
  int rowoff, coloff;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  int statusmsg_interval;
  char processing;
  char netProcessing;
};

struct editorConfig E;
netConfig netConf;

/*** prototypes ***/
void editorSetStatusMessage(int, const char *, ...);
void editorRefreshScreen();
char *editorPrompt(char *, size_t);
void delay(time_t);
int multipleChoice(const char *, int, ...);
void createSession();
int connectToServer();
int serverSend(char* buf, size_t len);
int disconnectFromServer();
char *serverReceive(int *len);
void setAndFreeze(char *msg, int sec);
void joinSession();
void listenServer();
void netInsertChar(int c, int cx, int cy);
void netInsertNewline(int cx, int cy);
void netDelChar(int cx, int cy);

/*** terminal ***/
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  struct termios raw;
  if (tcgetattr(STDIN_FILENO, &raw) == -1) {
    die("tcgetattr");
  }
  E.orig_termios = raw;
  atexit(disableRawMode);

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
      if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
      if (buf[i] == 'R') break;
      i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
      ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
          return -1;
        }
        return getCursorPosition(rows, cols);
  } else {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') rx += (COLED_TAB_STOP - 1) - (rx % COLED_TAB_STOP);
    rx++;
  }

  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;

  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (COLED_TAB_STOP - 1) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % COLED_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }

  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
	if (netConf.connected) {//more complex condition?
		char cmd[] = "char";
		char msg[sizeof(cmd) + 3];
		snprintf(msg, sizeof msg, "%s %c ", cmd, c);
		serverSend(msg, sizeof(msg)-1);
		
		size_t maxnumlen = 10;
   	char cx[maxnumlen + 2];
    size_t l = snprintf(cx, sizeof cx, "%d ", E.cx);
    serverSend(cx, l);
    
    char cy[maxnumlen + 2];
    l = snprintf(cy, sizeof cy, "%d\n", E.cy);
    serverSend(cy, l);
	}
	
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
	if (netConf.connected) {
		char cmd[] = "newline"; 
		serverSend(cmd, sizeof(cmd)-1);
		serverSend(" ", 1);
		
		size_t maxnumlen = 10;
   	char cx[maxnumlen + 2];
    size_t l = snprintf(cx, sizeof cx, "%d ", E.cx);
    serverSend(cx, l);
    
    char cy[maxnumlen + 2];
    l = snprintf(cy, sizeof cy, "%d\n", E.cy);
    serverSend(cy, l);
	}
	
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->chars = realloc(row->chars, E.cx + 1); //freeing &row->chars[E.cx]
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);

  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  
  if (netConf.connected) {
  	char cmd[] = "delete"; 
		serverSend(cmd, sizeof(cmd)-1);
		serverSend(" ", 1);
		
		size_t maxnumlen = 10;
   	char cx[maxnumlen + 2];
    size_t l = snprintf(cx, sizeof cx, "%d ", E.cx);
    serverSend(cx, l);
    
    char cy[maxnumlen + 2];
    l = snprintf(cy, sizeof cy, "%d\n", E.cy);
    serverSend(cy, l);
  }

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;

    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", 0);
    if (E.filename == NULL) {
      editorSetStatusMessage(5, "Save aborted");
      return;
    }
  }
  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        editorSetStatusMessage(5, "%d bytes written to disk", len);
        E.dirty = 0;
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage(5, "Can't save! I/O error: %s", strerror(errno));
}

/*** network ***/

void network() {

  const char *msg = "Save file? y/n: %s";
  while (1) {
    int decision = multipleChoice(msg, 2, "y", "n");
    if (decision == 0) {
      editorSave();
      editorRefreshScreen();
      delay(2);
      break;
    }

    if (decision == 1) {
      break;
    }

    msg = "Invalid message. y/n: %s";
  }

  editorRefreshScreen();

  msg = "Create session or join? c/j: %s (ESC to cancel)";
  while (1) {
    int decision = multipleChoice(msg, 2, "c", "j");
    if (decision == 0) {
      createSession();
      break;
    }

    if (decision == 1) {
      joinSession();
      break;
    }

    if (decision == 3) {
      break;
    }

    msg = "Invalid message. c/j: %s";
  }

}

void createSession() {
  if (!netConf.connected && connectToServer() < 0) {
    setAndFreeze("Error with connecting to server", 4);
    return;
  }

  char *pass = editorPrompt("Set password: %s (ESC to cancel)", MAXPASSLEN);
  if (!pass) return;

  char *cmd = "create";
  char msg[strlen(cmd) + strlen(pass) + 3];
  size_t l = snprintf(msg, sizeof(msg),
    "%s %s\n", cmd, pass);

  //create session request to server with password
  editorSetStatusMessage(2, "Sending...");
  editorRefreshScreen();

  if (serverSend(msg, sizeof(msg) - 1) < 0) {
    setAndFreeze("Send error", 2);
    netConf.connected = 0;
    return;
  }

  editorSetStatusMessage(2, "Receiving...");
  editorRefreshScreen();

  int anslen = 0;
  char *id = serverReceive(&anslen);
  if (anslen <= 0) {
    setAndFreeze("Receive error", 5);
    netConf.connected = 0;
    return;
  }

  free(netConf.id);
  free(netConf.pass);
  netConf.id = id;
  netConf.pass = pass;

  cmd = "your id is";
  char msg2[strlen(cmd) + strlen(id) + 2];
  l = snprintf(msg2, sizeof(msg2), "%s %s", cmd, id);
  listenServer();
  setAndFreeze(msg2, 5);
}

void joinSession() {
  if (!netConf.connected && connectToServer() < 0) {
    setAndFreeze("Error with connecting to server", 4);
    return;
  }

  char needid = 1, needpass = 1;
  char *id, *pass;

  while (1) {
    if (needid) {
      id = editorPrompt("Enter id: %s (ESC to cancel)", IDLEN);
      if (!id) return;
    }

    if (needpass) {
      pass = editorPrompt("Enter password: %s (ESC to cancel)", MAXPASSLEN);
      if (!pass) return;
    }

    char *cmd = "join";
    char msg[strlen(cmd) + strlen(id) + strlen(pass) + 4];
    size_t l = snprintf(msg, sizeof(msg),
      "%s %s %s\n", cmd, id, pass);

    editorSetStatusMessage(2, "Sending...");
    editorRefreshScreen();

    if (serverSend(msg, sizeof(msg)-1) < 0) {
      setAndFreeze("Send error", 2);
      netConf.connected = 0;
      free(id);
      free(pass);
      return;
    }

    editorSetStatusMessage(2, "Receiving...");
    editorRefreshScreen();

    int anslen = 0;
    char *ans = serverReceive(&anslen);
    if (anslen <= 0) {
      setAndFreeze("Receive error", 5);
      netConf.connected = 0;
      free(id);
      free(pass);
      return;
    }

    if (strcmp(ans, "invalid id") == 0) {
      setAndFreeze("Invalid id", 4);
      needid = 1;
      needpass = 0;
      continue;
    }

    if (strcmp(ans, "invalid pass") == 0) {
      setAndFreeze("Invalid pass", 4);
      needid = 0;
      needpass = 1;
      continue;
    }

    if (strcmp(ans, "success") == 0) {
      editorSetStatusMessage(4, "Successful join");
      editorRefreshScreen();
      break;
    }

    setAndFreeze("Unknown server response", 4);
    return;
  }

  free(netConf.id);
  free(netConf.pass);
  netConf.id = id;
  netConf.pass = pass;

  int sizeN = 0;
  char *numrowsbuf = serverReceive(&sizeN);
  if (sizeN <= 0) {
    free(numrowsbuf);
    setAndFreeze("Receive numrows error", 4);
    free(id);
    free(pass);
    netConf.id = NULL;
    netConf.pass = NULL;
    netConf.connected = 0;
    return;
  }
  
  int numrows = atoi(numrowsbuf);
  int oldnum = E.numrows;
  for (int i = 0; i < numrows; i++) {
    int anslen = 0;
    char *ans = serverReceive(&anslen);
    if (anslen < 0) {
      free(numrowsbuf);
      free(ans);
      free(id);
      free(pass);
      netConf.id = NULL;
      netConf.pass = NULL;
      netConf.connected = 0;
      setAndFreeze("Receive rows error", 4);
      return;
    }

    if (i < oldnum) {
      editorDelRow(i);
    }
    editorInsertRow(i, ans, anslen);
    free(ans);
  }
  
  for (int i = oldnum - 1; numrows >= 0 && i >= numrows; i--) {
  	editorDelRow(i);
  }
  
  //cx, cy = 0, 0?
  editorRefreshScreen();
  free(numrowsbuf);
  listenServer();
}

int connectToServer() {
  netConf.server = socket(AF_INET, SOCK_STREAM, 0);
  if (netConf.server < 0) {
    setAndFreeze("Socket error", 2);
    return netConf.server;
  }

  struct sockaddr_in peer;
  peer.sin_family = AF_INET;
  peer.sin_port = htons(netConf.serverPort);
  peer.sin_addr.s_addr = inet_addr(netConf.serverIp);

  int res = connect(netConf.server, (const struct sockaddr *) &peer, sizeof(peer));

  if (res < 0) {
    setAndFreeze("Connect error", 2);
    return res;
  }
  netConf.connected = 1;

  return 1;
}

int disconnectFromServer() {
  netConf.connected = 0;
  int res = shutdown(netConf.server, 1);
  if (res < 0) {
    setAndFreeze("Disconnect error", 2);
    return res;
  }

  res = close(netConf.server);
  if (res < 0) {
    setAndFreeze("Close socket error", 2);
    return res;
  }

  return 1;
}

int serverSend(char *buf, size_t len) {
  int count = 0;
  while (len > 0) {
    /*if (count > 0) {
      editorSetStatusMessage(5, "%s send count %d", buf, count);
      editorRefreshScreen();
      delay(3);
    }*/
    int i = send(netConf.server, buf, len, 0);
    if (i < 1) return -1;
    buf += i;
    len -= i;
    count++;
  }
  return 1;
}

char *serverReceive(int *len) {
  size_t bufsize = 32;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = 0;

  while (1) {
    int res = recv(netConf.server, buf + (buflen++), 1, 0);
    buf[buflen] = 0;
    if (res <= 0) {
      if (len != NULL) *len = res;
      return buf;
    }
    if (buf[buflen-1] == '\n') {
      if (len != NULL) *len = buflen-1;
      buf[buflen-1] = 0;
      return buf;
      // return realloc(buf, buflen); //memory leak? realloc(buflen)
    }
    if (buflen == bufsize - 1) {
      bufsize *= 2;
      buf = realloc(buf, bufsize);
    }
  }
}

char **splitStr(char *str, size_t *len) {
  char *token;
  size_t arrlen = 10;
  *len = 0;
  char **arr = malloc(arrlen * sizeof(char*));
  while ((token = strsep(&str, ",")) != NULL) {
    arr[*len] = strdup(token);
    (*len)++;
    if ((*len) == arrlen) {
      arrlen *= 2;
      arr = realloc(arr, arrlen);
    }
  }

  return arr;
}

void *threadListen() {
  time_t *lastTry = NULL;
  while (1) {
    E.netProcessing = 0;//only if buffer is empty
    if (!netConf.connected && (!lastTry || *lastTry + netConf.connectInterval < time(NULL))) {
      *lastTry = time(NULL);
      connectToServer();
    }
    int anslen = 0;
    char *ans = serverReceive(&anslen);
    if (anslen <= 0) continue;
    while (E.processing);
    E.netProcessing = 1;

    if (strcmp(ans, "request") == 0) {
      editorSetStatusMessage(2, "Received request");
      editorRefreshScreen();
      
      int res = serverSend("response\n", 9);
      if (res <= 0) {
        editorSetStatusMessage(2, "Server send response error");
        editorRefreshScreen();
        free(ans);
        netConf.connected = 0;
        continue;
      }
      
      size_t maxnumlen = 10;
    	char msg[maxnumlen + 1];
    	size_t l = snprintf(msg, sizeof(msg),
      "%d", E.numrows);
      res = serverSend(msg, l);
      if (res <= 0) {
        editorSetStatusMessage(2, "Server send numrows error");
        editorRefreshScreen();
        free(ans);
        netConf.connected = 0;
        continue;
      }
      res = serverSend("\n", 1);
      if (res <= 0) {
        editorSetStatusMessage(2, "Server send \\n error");
        editorRefreshScreen();
        free(ans);
        netConf.connected = 0;
        continue;
      }
      
      int i;
      for (i = 0; i < E.numrows; i++) {
        res = serverSend(E.row[i].chars, E.row[i].size);
        if (res <= 0) {
          editorSetStatusMessage(2, "Server send %d row error", i);
          editorRefreshScreen();
          delay(4);
          netConf.connected = 0;
          break;
        }
        res = serverSend("\n", 1);
        if (res <= 0) {
          editorSetStatusMessage(2, "Server send %d row error \\n", i);
          editorRefreshScreen();
          delay(4);
          netConf.connected = 0;
          break;
        }
        //setAndFreeze(E.row[i].chars, 3);
      }
      editorSetStatusMessage(5, "Successful send %d rows", i);
    } else if (strcmp(ans, "char") == 0) {
    	char *c = serverReceive(NULL);  //check for errors
    	
    	char *cx = serverReceive(NULL);
    	char *cy = serverReceive(NULL);
    	netInsertChar(*c, atoi(cx), atoi(cy));
    	free(c);
    	free(cx);
    	free(cy);
    } else if (strcmp(ans, "newline") == 0) {
    	char *cx = serverReceive(NULL);
    	char *cy = serverReceive(NULL);
    	
    	netInsertNewline(atoi(cx), atoi(cy));
    	free(cx);
    	free(cy);
    } else if (strcmp(ans, "delete") == 0) {
    	char *cx = serverReceive(NULL);
    	char *cy = serverReceive(NULL);
    	
    	netDelChar(atoi(cx), atoi(cy));
    	free(cx);
    	free(cy);
    }

    free(ans);
    editorRefreshScreen();
    //cmd and refresh
  }
}

void listenServer() {
  pthread_t tid;
  pthread_create(&tid, NULL, threadListen, NULL);
  pthread_detach(tid);
}

void netInsertChar(int c, int cx, int cy) {
	if (cy > E.numrows) return;
  if (cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
	editorRowInsertChar(&E.row[cy], cx, c);
}

void netInsertNewline(int cx, int cy) {
	if (cy > E.numrows) return;
	if (cy == E.numrows && cx != 0) return;
	if (E.row[cy].size < cx) return;
	
	if (cx == 0) {
    editorInsertRow(cy, "", 0);
  } else {
    erow *row = &E.row[cy];
    editorInsertRow(cy + 1, &row->chars[cx], row->size - cx);
    row = &E.row[cy];
    row->chars = realloc(row->chars, cx + 1); //freeing &row->chars[E.cx]
    row->size = cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
}

void netDelChar(int cx, int cy) {
	if (cy >= E.numrows) return;
  if (cx == 0 && cy == 0) return;
	if (cx > E.row[cy].size) return;
	
  erow *row = &E.row[cy];
  if (cx > 0) {
    editorRowDelChar(row, cx - 1);
  } else {
    editorRowAppendString(&E.row[cy - 1], row->chars, row->size);
    editorDelRow(cy);
  }
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0};

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;

  memcpy(new + ab->len, s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "COLED editor -- version %s", COLED_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, E.row[filerow].render + E.coloff, len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %.11s",
    E.filename ? E.filename : "[No name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d:%d",
    E.cy + 1, E.rx + 1);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    }
    abAppend(ab, " ", 1);
    len++;
  }

  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < E.statusmsg_interval) {
    abAppend(ab, E.statusmsg, msglen);
  } else {
  	msglen = 0;
  }
    
	while (msglen < E.screencols) {
    abAppend(ab, " ", 1);
    msglen++;
  }
  abAppend(ab, "\x1b[m", 3);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(int sec, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
  E.statusmsg_interval = sec;
}

void setAndFreeze(char *msg, int sec) {
  editorSetStatusMessage(sec, msg);
  editorRefreshScreen();
  delay(sec);
  editorRefreshScreen();
}

/*** input ***/

void delay(time_t t) {
  time_t curr = time(NULL);
  while (time(NULL) - curr < t);
}

int multipleChoice(const char *msg, int n, ...) {
  va_list ap;
  va_start(ap, n);
  const char **choices = malloc(sizeof(const char*) * n);
  int i = 0;
  while (n--) {
    choices[i] = va_arg(ap, const char*);
    i++;
  }

  char *buf = editorPrompt(msg, 0);
  n = i;
  if (!buf) return n+1;

  for (i = 0; i < n; i++) {
    if (strcmp(choices[i], buf) == 0) break;
  }
  free(buf);
  free(choices);
  va_end(ap);
  return i;
}

char *editorPrompt(char *prompt, size_t maxlen) {
  size_t bufsize = 32;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editorSetStatusMessage(5, prompt, buf);
    editorRefreshScreen();
    int c = editorReadKey();

    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = 0;
    } else if (c == '\x1b') {
      editorSetStatusMessage(5, "Leaving...");
      editorRefreshScreen();
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage(5, "");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (maxlen > 0 && maxlen == buflen) continue;
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
          E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  static int quit_times = COLED_QUIT_TIMES;

  int c = editorReadKey();

  while (E.netProcessing);
  E.processing = 1;

  switch (c) {
    case '\r':
      editorInsertNewline();
      break;

    case CTRL_KEY('q'):
      if (quit_times > 0 && E.dirty) {
        editorSetStatusMessage(5, "WARNING!!! File has unsaved changes. "
        "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case CTRL_KEY('n'):
      network();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }
        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_RIGHT:
    case ARROW_LEFT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(c);
      break;
  }

  E.processing = 0;
  quit_times = COLED_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = 0;
  E.statusmsg_time = 0;
  E.processing = 0;
  E.netProcessing = 0;

  if (getWindowSize(&E.screenHeight, &E.screenWidth) == -1) {
    die("getWindowSize");
  }

  E.screenrows = E.screenHeight - 2;  //for status and message bar
  E.screencols = E.screenWidth;
}
void initNet() {
  netConf.serverIp = "127.0.0.1";
  netConf.serverPort = 3018;
  netConf.server = -1;
  netConf.connected = 0;
  netConf.id = NULL;
  netConf.pass = NULL;
  netConf.connectInterval = 25;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  initNet();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage(5, "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-N = network");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}


package main

import (
	"bufio"
	"fmt"
	"log"
	"os"
	"net"
	"strings"
	"strconv"
	"github.com/rs/xid"
)

type Session struct {
	id, pass string
	participants map[*net.Conn]struct{}
	host *net.Conn
}

func (s *Session) Add(c *net.Conn) {
	s.participants[c] = struct{}{}
}

func (s *Session) Delete(c *net.Conn) {
	delete(s.participants, c)
	if s.Empty() {
		delete(sessions, s.id)
		log.Printf("Deleting session %s\n", s.id)
	} else if s.host == c {
		for newhost := range s.participants {
			s.host = newhost
			log.Printf("New host of %s is %s", s.id, (*newhost).RemoteAddr().String())
			break
		}
	}
}

func (s *Session) Empty() bool {
	return len(s.participants) == 0
}

func (s *Session) Init() {
	s.participants = make(map[*net.Conn]struct{})
}

const (
	connHost = "localhost"
	connPort = "3018"
	connType = "tcp"
)

var (
	sessions map[string]*Session
	copyRows chan []byte = make(chan []byte, 1)
	rowsLen chan int = make(chan int)
)

func main() {
	sessions = make(map[string]*Session)
	
	fmt.Println("Starting " + connType + " server on " + connHost + ":" + connPort)
	l, err := net.Listen(connType, connHost+":"+connPort)

	if err != nil {
		fmt.Println("Error listening:", err.Error())
		os.Exit(1)
	}
	defer l.Close()

	for {
		c, err := l.Accept()
		if err != nil {
			fmt.Println("Error connecting:", err.Error())
			return
		}

		fmt.Println("Client " + c.RemoteAddr().String() + " connected.")
		go handleConn(c)
	}
}

func handleConn(c net.Conn) {
	var currentSess *Session = nil
	reader := bufio.NewReader(c)
	connected := false
	for true {
		buffer, err := reader.ReadBytes('\n')
		log.Print("msg: " + string(buffer))
		
		if err != nil {
			currentSess.Delete(&c)
			log.Printf("Client %s left: ", c.RemoteAddr().String())
			fmt.Println(err)
			c.Close()
			return
		}
		
		log.Println(buffer)
		log.Printf("Len of buffer is %d\n", len(buffer))
		if len(buffer) == 0 {
			c.Write([]byte("Unknown command\n"))
			continue;
		}
		
		params := SplitString(string(buffer[:len(buffer)-1]), ' ')
		log.Println(params)
		log.Printf("Len of params is %d\n", len(params))
		if len(params) == 2 && params[0] == "create" {
			currentSess = &Session{}
			currentSess.Init()
			
			guid := xid.New()
			currentSess.id = guid.String()
			currentSess.pass = params[1];
			sessions[currentSess.id] = currentSess
			currentSess.Add(&c)
			currentSess.host = &c
			c.Write([]byte(guid.String() + "\n"))
			connected = true
			log.Println(guid.String())
		} else if len(params) == 3 && params[0] == "join" {
				currSess, ok := sessions[params[1]]
				currentSess = currSess
				if !ok {
					c.Write([]byte("invalid id\n"))
					continue
				}
				if params[2] != currentSess.pass {
					c.Write([]byte("invalid pass\n"))
					continue
				}

				currentSess.Add(&c)
				c.Write([]byte("success\n"))

				host := *currentSess.host
				host.Write([]byte("request\n"))
				bsrowsnum := <- copyRows
				rowsnum, err := strconv.Atoi(string(bsrowsnum[:len(bsrowsnum)-1]))
				if err != nil {
					log.Println("err ->")
					log.Println(err)
				}
				rowsLen <- rowsnum

				c.Write(bsrowsnum)
				for i := 0; i < rowsnum; i++ {
					row := <- copyRows
					c.Write(row)
				}
				connected = true
		} else if len(params) == 1 && strings.Compare(params[0], "response") == 0 && connected && currentSess.host == &c {
			log.Println("Received response")
			bsrowsnum, err := reader.ReadBytes('\n')
			if err != nil {
				log.Printf("Client %s left: ", c.RemoteAddr().String())
				fmt.Println(err)
				c.Close()
				return
			}
			log.Println(bsrowsnum)		
			copyRows <- bsrowsnum
			rowsnum := <- rowsLen
			log.Println(rowsnum)
			for i := 0; i < rowsnum; i++ {
				row, err := reader.ReadBytes('\n')
				if err != nil {
					log.Println("Error receive rows")
					break;
				}
				log.Printf("row %d: %s", i, string(row))
				copyRows <- row
			}
		} else if connected {
		 	if len(params) == 4 && params[0] == "char" || 
		 	 len(params) == 3 && (params[0] == "newline" || params[0] == "delete") {
		 	 	log.Println("Valid cmd")
		 	 	for part := range currentSess.participants {
		 	 		if part == &c {continue}
		 	 		for _, param := range params {
		 	 			(*part).Write([]byte(param))
		 	 			(*part).Write([]byte("\n"))
		 	 		}
		 	 		log.Printf("Writing to %s", (*part).RemoteAddr().String())
		 	 	}
		 	 }
		}
	}
}

func SplitString(str string, sep rune) []string {
	strs := make([]string, 0)
	var curr strings.Builder
	
	for _, c := range []rune(str) {
		if c == sep {
			if curr.Len() != 0 {
				strs = append(strs, curr.String())
				curr.Reset()
				continue
			}
		}
		curr.Grow(curr.Cap() + 1)
		curr.WriteRune(c)
	}
	
	if curr.Len() != 0 {
		strs = append(strs, curr.String())
	}
	return strs
}


# Collaborative Console Text Editor
Coled is the expansion of [Kilo](https://github.com/antirez/kilo) console text editor created with the lead of [184 steps guide](https://viewsourcecode.org/snaptoken/kilo/index.html) adding the ability of collaborative text editing!
## Installation
```Bash
git clone https://github.com/frussian/Coled.git
go get github.com/rs/xid
make
go build server.go
```
Editor works only on Linux distros and on Linux subsystem for Windows

Client connects to localhost:3018 from dynamic port by default

## License
This project is licensed under the MIT license. See [LICENSE](LICENSE) for details and 3rd party licenses.

# Antelope Search
"antelope" is a super fast search engine written in C/C++. Initially it was developed by Andrew Trotman who teaches in University of Otago.

## File Structure
### The files that are different with the upstream
* antelope
* version.h

## Build Antelope
```
cd [path/to/antelope]
./autogen.sh
mkdir -p build/{release,debug}

## DEBUG BUILD WITH g++
## In a new terminal
cd build/debug
source ../../scripts/setdebugenv.sh
../../configure
make
sudo make install

## RELEASE BUILD
## In a new terminal
cd build/release
../../configure
make
sudo make install

## DEBUG BUILD WITH clang++
## In a new terminal
cd build/debug
source ../../scripts/setdebugenv.clang
../../configure CC=clang++ CXX=clang++
make
sudo make install
```

## Usage Examples
### Indexing / Search with TREC data
[Zettair](http://www.seg.rmit.edu.au/zettair/download.html) search engine provides a pre-compiled text of novel - "Moby Dick" in TREC format.

### To index:
```
cd [path/to/zettair/txt]
index -rtrec moby.txt
```

### To search:
```
cd [path/to/zettair/txt]
antelope
```

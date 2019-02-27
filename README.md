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

At the moment, antelope supports indexing documents in structural formats such as csv, xml, wiki, etc.

### Indexing / Search with TREC data
[Zettair](http://www.seg.rmit.edu.au/zettair/download.html) search engine provides a pre-compiled text of novel - "Moby Dick" in TREC format.

#### To index documents in TREC format:
```
cd [path/to/zettair/txt]
index -rtrec moby.txt
```

#### To index Wiki style documents as as Wikipedia articles:

For the xml documents, we need to specify the root tag of a document, then the title tag.

```
cd [path/to/wikipedia/xml/files]
index -rtrec:tag:page:title file1 file2 file3 ...
```

Or, index all documents under a folder:

```
cd [path/to/wikipedia/xml/files]
index -rtrec:tag:page:title folder1 ...
```

#### To search:
```
cd [path/to/zettair/txt]
antelope
```

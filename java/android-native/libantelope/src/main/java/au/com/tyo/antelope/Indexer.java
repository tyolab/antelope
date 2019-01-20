package au.com.tyo.antelope;

import java.io.IOException;

import au.com.tyo.antelope.jni.ATIRE_indexer;

public class Indexer {
    static {
	    System.loadLibrary("antelope_jni");
	}
    
    public void index(String what) throws IOException {
//    	atire_apis.atire_index(what);
    	ATIRE_indexer indexer = new ATIRE_indexer();
    	indexer.init("-Rt[FOLDER:YMD]+-kt");
    	
    	byte[] bytes = Utils.readFileIntoBytes(what);
    	int i = 0;
//    	SgmlNode root = new SgmlNode();
//    	root.parse(bytes);
//    	
//    	for (int i = 0; i < root.countChildren(); ++i) {
//    		SgmlNode child = root.getChild(i);
    		String id = String.valueOf(i);
//    		String text = child.toText();
//    		indexer.index_document(id, text);
//    	}
    	
    	String text = new String(bytes);
    	int start = 0;
    	int end = 0;
    	while ((start = text.indexOf("<DOC>", end)) > -1) {
    		end = text.indexOf("</DOC>", start);
    		
    		if (end > 0) {
    			String doc = text.substring(start, end + 6);
    			indexer.index_document(id, doc);
    		}
    		else {
    			System.err.println("ill-formed xml text");
    			System.exit(-1);
    		}
    		++i;
    	}
    	
    	int ret = indexer.finish();
    	
    	if (ret == 1)
    		System.err.println("successfully indexed");
    	else
    		System.err.println("indexing failed");
    	
    	indexer.delete();
    }
    
    public static void usage() {
    	System.out.println("usage: program option1;option2;...;file1;file2;file3;...");
    }

	/**
	 * @param args
	 */
	public static void main(String[] args) {
		if (args.length == 0) {
			usage();
			System.exit(-1);
		}
		try {
			new Indexer().index(args[0]);
		} catch (IOException e) {
			e.printStackTrace();
		}
	}

}

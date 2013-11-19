package org.atire;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.ConnectException;
import java.net.Socket;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.HashMap;

import org.atire.swig.ATIRE_API_remote;

import au.com.tyo.parser.SgmlNode;

public class AtireRemoteClient {
	
	public static final String DEFAULT_SERVER = "localhost";
	
	public static final int DEFAULT_PORT  = 8088;
	
	private ATIRE_API_remote socket;
	
	private int port;
	
	private String serverAddress;
	
	private HashMap<String, SearchResult> results;
	
    static {
	    System.loadLibrary("atire_jni");
	}
	
	public static class SearchResult {
		String name;
		
		int rank;
		
		String id;
		
		double rsv;
		
		public String toString() {
			return "rank: " + rank + ", " + "name: " + name + ", " + "id: " + id + ", " + "rsv: " + rsv;
		}
	}

	public AtireRemoteClient() {
		port = DEFAULT_PORT;
		serverAddress = DEFAULT_SERVER;
		results = new HashMap<String, SearchResult>();
		init();
	}

	private void init() {
		socket = new ATIRE_API_remote();
		initializeSocket();
	}
	
	public void initializeSocket() {
		socket.open(serverAddress + ":" + port);
	}
	
	public void close() {
		socket.close();
	}
	
	public void destroy() {
		socket.delete();
	}
	
	/*
	BETWEEN()
	---------
	written by Andrew Trotman
*/
	private String between(String source, String open_tag, String close_tag)
	{
//		char *start,*finish;
//		
//		if ((start = strstr((char *)source, (char *)open_tag)) == NULL)
//			return NULL;
//		
//		start += strlen(open_tag);
//		
//		if ((finish = strstr(start, close_tag)) == NULL)
//			return NULL;
//		
//		return strnnew(start, finish - start);
		return "";
	}
	
	public ArrayList<String> search(String query) throws ConnectException {
		ArrayList<String> list = new ArrayList<String>();
		
		results.clear();
		
//			out.writeBytes(query + "\n");
			
		String line = socket.search(query, 1, 10);
		
//			while ((line = in.readLine()) != null) {
		
		SgmlNode node = new SgmlNode();
		node.parse(line.getBytes());
		SearchResult result = new SearchResult();
		for (int i = 0; i < node.countChildren(); ++i) {
			SgmlNode child = node.getChild(i);
			
			if (child.getName().equalsIgnoreCase("id")) {
				result.id = child.getText();
			}
			else 	if (child.getName().equalsIgnoreCase("name")) {
				result.name = child.getText();
			}
			else 	if (child.getName().equalsIgnoreCase("rank")) {
				result.rank = Integer.parseInt(child.getText());
			}
			else 	if (child.getName().equalsIgnoreCase("rsv")) {
				result.rsv = Double.parseDouble(child.getText());
			}
		}
    	System.err.println("rvsc:" + result.toString());
		results.put(result.name, result);
		list.add(result.name);
		return list;	
	}
	
	public void tryAgain() {
		
	}

	public static void main(String[] args) {
		
		AtireRemoteClient atire = new AtireRemoteClient();
		
		if (args.length == 0) {
			BufferedReader stdIn =
	                new BufferedReader(new InputStreamReader(System.in));
	            String fromUser;
 
            try {
				while ((fromUser = stdIn.readLine()) != null) {
				    System.out.println("Search: " + fromUser);
				    if (fromUser.equals("Bye."))
				        break;
				     
				    ArrayList<String> results = null;
				    
				    try {
				    	results = atire.search(fromUser);
				    }
				    catch (ConnectException connEx) {
				    	System.err.println(connEx.getMessage());
				    	atire.initializeSocket();
				    	
				    	try {
				    		results = atire.search(fromUser);
				    	}
				    	 catch (ConnectException connEx2) {
				    		 System.err.println("re-attempt connection failed");
				    		 System.exit(-1);
				    	 }
				    }
				    catch (SocketException socketEx) {
				    	atire.initializeSocket();
				    	
				    	try {
				    		results = atire.search(fromUser);
				    	}
				    	 catch (ConnectException connEx2) {
				    		 System.err.println("re-attempt connection failed");
				    		 System.exit(-1);
				    	 }
				    }
				    
				    if (results != null) {
				    	System.out.println("Results: ");
				    	for (int i = 0; i < results.size(); ++i) 
				    		System.out.println(results.get(i));
				    	System.out.println("");
				    }
				}
			} catch (IOException e) {
				e.printStackTrace();
			}
		} else
			try {
				atire.search(args[0]);
			} catch (ConnectException e) {
				e.printStackTrace();
			}
		atire.close();
		atire.destroy();
	}
}

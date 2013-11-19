package org.atire;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.UnsupportedEncodingException;
import java.net.ConnectException;
import java.net.Socket;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;

import org.atire.swig.ATIRE_API_remote;

import au.com.tyo.parser.SgmlNode;
import au.com.tyo.utils.ByteArrayKMP;

public class AtireRemoteClient {
	
	public static final String DEFAULT_SERVER = "localhost";
	
	public static final int DEFAULT_PORT  = 8088;
	
	private ATIRE_API_remote socket;
	
	private int port;
	
	private String serverAddress;
	
	private HashMap<String, SearchResult> results;
	
	private int hits;
	
	private ByteArrayKMP kmpSearch;
	
	private byte[] buffer;
	
	private int index;
	
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
		ByteArrayKMP kmpSearch = new ByteArrayKMP();
		
		port = DEFAULT_PORT;
		serverAddress = DEFAULT_SERVER;
		results = new HashMap<String, SearchResult>();
		init();
	}

	private void init() {
		hits = 0;
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
	
	private String between(String open_tag, String close_tag) {
		return between(open_tag, close_tag, 0);
	}
	
	/*
	BETWEEN()
	---------
	written by Andrew Trotman
*/
	private String between(String open_tag, String close_tag, int pos)	{
		String got = "";
		int start = pos, finish;
		
		if ((start = kmpSearch.search(buffer, pos)) < 0)
			return got;
		
		start += open_tag.length();
		
		kmpSearch.setPattern(close_tag.getBytes());
		if ((finish = kmpSearch.search(buffer, start)) < 0)
			return got;
//		
		if (pos > 0)
			index = finish;
		
		try {
			got = new String(Arrays.copyOfRange(buffer, start, finish), "UTF-8");
		} catch (UnsupportedEncodingException e) {
			e.printStackTrace();
			got = new String(Arrays.copyOfRange(buffer, start, finish));
		}
		
		return got;
	}
	
	public ArrayList<String> search(String query) throws ConnectException {
		ArrayList<String> list = new ArrayList<String>();
		long hits;
		
		results.clear();
		
//			out.writeBytes(query + "\n");
			
		String line = socket.search(query, 1, 10);
		
    	System.err.println("rvsc:" + line);
		
//			while ((line = in.readLine()) != null) {
    	
    	buffer = line.getBytes();
		
    	long atire_found = hits = Long.parseLong(between("<numhits>", "</numhits>"));

    	index = 0;
    	int current = index;
    	for (current = 0; current < hits; current++)  {
    		kmpSearch.setPattern("<hit>".getBytes());
    		SearchResult result = new SearchResult();
    		index = kmpSearch.search(buffer, index);
    		result.rank = Integer.parseInt(between("<rank>", "</rank>", index));
    		result.id = between("<id>", "</id>", index);
    		result.name = between("<name>", "</name>", index);
    		result.rsv = Double.parseDouble(between("<rsv>", "</rsv>", index));
    		
    		results.put(result.name, result);
    		list.add(result.name);
    	}

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

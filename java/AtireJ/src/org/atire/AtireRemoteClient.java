package org.atire;

import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.UnsupportedEncodingException;
import java.net.ConnectException;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;

import org.atire.swig.ATIRE_API_remote;
import org.atire.swig.SWIGTYPE_p_long_long;

import au.com.tyo.utils.ByteArrayKMP;

public class AtireRemoteClient {
	
	public static final String DEFAULT_SERVER = "localhost";
	
	public static final int DEFAULT_PORT  = 8088;
	
	public static final int DEFAULT_PAGE_SIZE = 10;
	
	private ATIRE_API_remote socket;
	
	private int port;
	
	private String serverAddress;
	
	private int pageSize;
	
	private HashMap<String, SearchResult> results;
	
	private long hits;
	
	private ByteArrayKMP kmpSearch;
	
	private byte[] buffer;
	
	private int index;
	
	public static String dylibName = "atire_jni";
	
	private static AtireRemoteClient instance;
	
	private boolean connected;
	
	public static class SearchResult {
		String name;
		
		int rank;
		
		long id;
		
		double rsv;
		
		public String toString() {
			return "rank: " + rank + ", " + "name: " + name + ", " + "id: " + id + ", " + "rsv: " + rsv;
		}
	}

	public AtireRemoteClient() {
		kmpSearch = new ByteArrayKMP();
		
		port = DEFAULT_PORT;
		serverAddress = DEFAULT_SERVER;
		pageSize = DEFAULT_PAGE_SIZE;
		connected = false;
		
		results = new HashMap<String, SearchResult>();
		init();
	}

	public static AtireRemoteClient getInstance() {
		if (instance == null)
			instance = new AtireRemoteClient();
		return instance;
	}

	private void init() {
		hits = 0;
		socket = new ATIRE_API_remote();
		initializeSocket();
	}
	
	public void initializeSocket() {
		int result = socket.open(serverAddress + ":" + port);
		if (result != 0)
			connected = true;
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
		
		kmpSearch.setPattern(open_tag.getBytes());
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
	
	public String sendCommand(String cmd) {
		return socket.send_command(cmd);
	}
	
	public ArrayList<String> listTitle(String query) {
		ArrayList<String> list = new ArrayList<String>();
		
		if (!connected) {
			this.initializeSocket();
		}
		
		results.clear();
		
		if (connected) {
			String result = socket.send_command(".listterm " + "TF:" + query);
			
			if (result != null && result.length() > 0) {
				InputStream is = new ByteArrayInputStream(result.getBytes());
				 
				BufferedReader br = new BufferedReader(new InputStreamReader(is));
			 
				String line;
				int count = 0;
				try {
					while ((line = br.readLine()) != null && line.length() > 3) {
						String[] tokens = line.substring(3).split(":");
						if (tokens != null && tokens.length > 1) {
							list.add(tokens[0]);
							
							SearchResult search = new SearchResult();
							search.name = tokens[0];
							try {
								search.id = Long.parseLong(tokens[tokens.length - 1]);
							}
							catch (Exception ex) {
								search.id = -1;
							}
							search.rank = ++count;
							
							results.put(tokens[0], search);
						}
					}
					
					br.close();
				} catch (IOException e) {
					e.printStackTrace();
				}
			}
		}
		if (list.size() == 0)
			try {
				return search(query);
			} catch (ConnectException e) {
				return list;
			}
		return list;
	}
	
	public ArrayList<String> search(String query) throws ConnectException {
		ArrayList<String> list = new ArrayList<String>();
		
		if (!connected) {
			this.initializeSocket();
		}
		
		results.clear();
		
		if (connected) {
		
//			out.writeBytes(query + "\n");
				
			String line = socket.search(query, 1, 10);
			
	    	System.err.println("rvsc:" + line);
			
	//			while ((line = in.readLine()) != null) {
	    	
	    	buffer = line.getBytes();
			
	    	hits = Long.parseLong(between("<numhits>", "</numhits>"));
	
	    	index = 0;
	    	int current = index;
	    	long upBound = Math.min(pageSize, hits);
	    	for (current = 0; current < pageSize; current++)  {
	    		kmpSearch.setPattern("<hit>".getBytes());
	    		SearchResult result = new SearchResult();
	    		index = kmpSearch.search(buffer, index);
	    		result.rank = Integer.parseInt(between("<rank>", "</rank>", index));
	    		result.id = Long.parseLong(between("<id>", "</id>", index));
	    		result.name = between("<name>", "</name>", index);
	    		result.rsv = Double.parseDouble(between("<rsv>", "</rsv>", index));
	    		
	    		results.put(result.name, result);
	    		list.add(result.name);
	    	}
		}

		return list;	
	}
	
	public String getDocumentAbstract(String name) {
		SearchResult search = results.get(name);
		String abs  = null;
		if (search != null && search .id > -1) {
			SWIGTYPE_p_long_long length =  null;
			String result = socket.get_document(search.id, length);
			
			byte[] bytes = result.getBytes();
			
			ByteArrayKMP kmp1 = new ByteArrayKMP("<abstract>".getBytes());
			ByteArrayKMP kmp2 = new ByteArrayKMP("</abstract>".getBytes());
			
			int start = -1, end = -1;
			
			start = kmp1.search(bytes);
			end = kmp2.search(bytes);
			
			if (start > -1 && end > -1 && end > start) {
				byte[] absBytes = Arrays.copyOfRange(bytes, start + 10, end);
				try {
					abs = new String(absBytes, "UTF-8");
				} catch (UnsupportedEncodingException e) {
					abs = new String(absBytes);
				}
			}
		}
		return abs;
	}

	public static void main(String[] args) {
		AtireRemoteClient.dylibName = "atire_jni";
	    System.loadLibrary(dylibName);
		
		AtireRemoteClient atire = new AtireRemoteClient();
	     
	    ArrayList<String> results = null;
		
		if (args.length == 0) {
			BufferedReader stdIn =
	                new BufferedReader(new InputStreamReader(System.in));
	            String fromUser;
 
            try {
				while ((fromUser = stdIn.readLine()) != null) {
				    System.out.println("Search: " + fromUser);
				    int pos = -1;
				    if (fromUser.equalsIgnoreCase("Bye"))
				        break;
				    else if ((pos = fromUser.indexOf("cmd ")) > -1) {
				    	String cmd = fromUser.substring(pos + 4);
				    	atire.sendCommand(cmd);
				    }
				    else {
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
				    }
				}
			} catch (IOException e) {
				e.printStackTrace();
			}
		} else
			try {
				results = atire.search(args[0]);
			} catch (ConnectException e) {
				e.printStackTrace();
			}
		atire.close();
//		atire.destroy();
			    
	    if (results != null) {
	    	System.out.println("Results: ");
	    	for (int i = 0; i < results.size(); ++i) 
	    		System.out.println(results.get(i));
	    	System.out.println("");
	    }
	}
}

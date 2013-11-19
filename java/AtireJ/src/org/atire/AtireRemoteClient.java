package org.atire;

import java.io.BufferedReader;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.Socket;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.HashMap;

public class AtireRemoteClient {
	
	public static final String DEFAULT_SERVER = "localhost";
	
	public static final int DEFAULT_PORT  = 8088;
	
	private Socket socket;
	
	private int port;
	
	private String serverAddress;
	
	private DataOutputStream out;
	
	private BufferedReader in;
	
	private HashMap<String, SearchResult> results;
	
	public static class SearchResult {
		String name;
		
		String id;
	}

	public AtireRemoteClient() {
		port = DEFAULT_PORT;
		serverAddress = DEFAULT_SERVER;
		results = new HashMap<String, SearchResult>();
		init();
	}

	private void init() {
		initializeSocket();
	}
	
	public void initializeSocket() {
		try {
			socket = new Socket(serverAddress, port);
			
//			out = new PrintWriter(socket.getOutputStream(), true);
//		    in = new BufferedReader(
//		        new InputStreamReader(socket.getInputStream()));
			in = new BufferedReader(new InputStreamReader(socket.getInputStream()));
			out = new DataOutputStream(socket.getOutputStream());
		} catch (UnknownHostException e) {
			e.printStackTrace();
		} catch (IOException e) {
			e.printStackTrace();
		}
	}
	
	public void close() {
		try {
			socket.close();
		} catch (IOException e) {
			e.printStackTrace();
		}
	}
	
	public ArrayList<String> search(String query) {
		ArrayList<String> list = new ArrayList<String>();
		
		results.clear();
		
		try {
			out.writeBytes(query);
		} catch (IOException e) {
			e.printStackTrace();
		}
		
		try {
			while (in.readLine() != null)
			
			return list;
		} catch (IOException e) {
			e.printStackTrace();
		}	
	}

}

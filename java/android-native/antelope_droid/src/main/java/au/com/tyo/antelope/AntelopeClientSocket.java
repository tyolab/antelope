package au.com.tyo.antelope;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.Socket;
import java.net.UnknownHostException;
import java.util.HashMap;
import java.util.List;

public class AntelopeClientSocket {
	
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
		
		int rank;
		
		String id;
		
		double rsv;
	}

	public AntelopeClientSocket() {
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
			in.close();
			out.close();
			socket.close();
		} catch (IOException e) {
			e.printStackTrace();
		}
	}
	
	// public List<String> search(String query) throws Exception {
	// 	ArrayList<String> list = new ArrayList<String>();
	//
	// 	results.clear();
	//
	// 	try {
	// 		out.writeBytes(query + "\n");
	//
	// 		String line;
	//
	// 		while ((line = in.readLine()) != null) {
	// 			SgmlNode node = new SgmlNode();
	// 			node.parse(line.getBytes());
	// 			SearchResult result = new SearchResult();
	// 			for (int i = 0; i < node.countChildren(); ++i) {
	// 				SgmlNode child = node.getChild(i);
	//
	// 				if (child.getName().equalsIgnoreCase("id")) {
	// 					result.id = child.getText();
	// 				}
	// 				else 	if (child.getName().equalsIgnoreCase("name")) {
	// 					result.name = child.getText();
	// 				}
	// 				else 	if (child.getName().equalsIgnoreCase("rank")) {
	// 					result.rank = Integer.parseInt(child.getText());
	// 				}
	// 				else 	if (child.getName().equalsIgnoreCase("rsv")) {
	// 					result.rsv = Double.parseDouble(child.getText());
	// 				}
	// 			}
	//
	// 			results.put(result.name, result);
	// 			list.add(result.name);
	// 		}
	//
	// 	} catch (IOException e) {
	// 		e.printStackTrace();
	// 	}
	// 	return list;
	// }
	
	public void tryAgain() {
		
	}

	// public static void main(String[] args) {
	//
	// 	AntelopeClientSocket atire = new AntelopeClientSocket();
	//
	// 	if (args.length == 0) {
	// 		BufferedReader stdIn =
	//                 new BufferedReader(new InputStreamReader(System.in));
	//             String fromUser;
    //
     //        try {
	// 			while ((fromUser = stdIn.readLine()) != null) {
	// 			    System.out.println("Search: " + fromUser);
	// 			    if (fromUser.equals("Bye."))
	// 			        break;
	//
	// 			    List<String> results = null;
	//
	// 			    try {
	// 			    	results = atire.search(fromUser);
	// 			    }
	// 			    catch (Exception connEx) {
	// 			    	System.err.println(connEx.getMessage());
	// 			    	atire.initializeSocket();
	//
	// 			    	try {
	// 			    		results = atire.search(fromUser);
	// 			    	}
	// 			    	 catch (Exception connEx2) {
	// 			    		 System.err.println("re-attempt connection failed");
	// 			    		 System.exit(-1);
	// 			    	 }
	// 			    }
	//
	// 			    if (results != null) {
	// 			    	System.out.println("Results: ");
	// 			    	for (int i = 0; i < results.size(); ++i)
	// 			    		System.out.println(results.get(i));
	// 			    	System.out.println("");
	// 			    }
	// 			}
	// 		} catch (IOException e) {
	// 			e.printStackTrace();
	// 		}
	// 	} else
	// 		try {
	// 			atire.search(args[0]);
	// 		} catch (Exception e) {
	// 			e.printStackTrace();
	// 		}
	// 	atire.close();
	// }
}

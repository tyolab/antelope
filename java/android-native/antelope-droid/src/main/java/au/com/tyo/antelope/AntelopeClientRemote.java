package au.com.tyo.antelope;

import java.util.ArrayList;
import java.util.List;

import au.com.tyo.antelope.jni.ATIRE_API_remote;
import au.com.tyo.antelope.jni.SWIGTYPE_p_long_long;

public class AntelopeClientRemote extends AntelopeClient {
	
	public static final String DEFAULT_SERVER = "localhost";
	
	public static final int DEFAULT_PORT  = 8088;
	
	private ATIRE_API_remote socket;
	
	private int port;
	
	private String serverAddress;

	private boolean connected;
	
//	public static class SearchResult {
//		String name;
//
//		int rank;
//
//		long id;
//
//		double rsv;
//
//	}

	public AntelopeClientRemote() {
		port = DEFAULT_PORT;
		serverAddress = DEFAULT_SERVER;
		connected = false;

		init();
	}


	private void init() {
		socket = new ATIRE_API_remote();
//		initializeSocket();
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

	public String sendCommand(String cmd) {
		return socket.send_command(cmd);
	}

	@Override
	public List<AntelopeDoc> listTitle(String query) {

		if (!connected) {
			this.initializeSocket();
		}

		if (connected)
			return super.listTitle(query);
		return new ArrayList<>();
	}

	@Override
    public AntelopeSearchResult search(String query, int pageIndex, int pageSize) throws Exception {
        List<AntelopeDoc> list = new ArrayList<>();

        results.clear();

//			out.writeBytes(query + "\n");
        String queryTerms = buildSearchQueries(query);
        String line = socket.search(queryTerms, 1, 10);

        System.err.println("rvsc:" + line);

        //			while ((line = in.readLine()) != null) {

        buffer = line.getBytes();

        hits = Long.parseLong(between("<numhits>", "</numhits>"));

        int current = 0;
        long upBound = Math.min(pageSize, hits);
        long base = (pageIndex - 1) * pageSize;
        int index = 0;
        for (current = 0; current < pageSize; current++)  {
            kmpSearch.setPattern("<hit>".getBytes());
            AntelopeDoc result = new AntelopeDoc();
            index = kmpSearch.search(buffer, index);
            result.title = between("<name>", "</name>", index);

            result.rank = Integer.parseInt(between("<rank>", "</rank>", index));
            result.docid = Long.parseLong(between("<id>", "</id>", index));
//            result.rsv = Double.parseDouble(between("<rsv>", "</rsv>", index));

            results.put(result.title, result);
            list.add(result);
        }

        preQuery = query;
        return list;
    }


    @Override
    protected String getDocumentInternal(long id) {
        if (id < 0)
            return "";
        SWIGTYPE_p_long_long length = null;
        return socket.get_document(id, length);
    }
}

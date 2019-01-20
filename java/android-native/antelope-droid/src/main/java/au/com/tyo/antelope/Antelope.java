package au.com.tyo.antelope;

import java.util.ArrayList;
import java.util.List;

import au.com.tyo.antelope.jni.ATIRE_API_result;
import au.com.tyo.antelope.jni.ATIRE_API_server;

public class Antelope extends AntelopeClient {

    private String opts;

    private ATIRE_API_server server;

    private AntelopeParser parser;

    public Antelope(String opts) {
        this.opts = opts;

        server = new ATIRE_API_server();

        server.set_params(opts);
        server.set_outchannel(3);
        server.initialize();
    }

    private static boolean nativeLibraryLoaded = false;

    public static void loadNativeLibrary() {
        if (!nativeLibraryLoaded) {
            System.loadLibrary(dylibName);
            nativeLibraryLoaded = true;
        }
    }

    public AntelopeParser getParser() {
        return parser;
    }

    public void setParser(AntelopeParser parser) {
        this.parser = parser;
    }

    public void start() {
        server.start();
    }

    public void stop() {
        server.finish();
    }

    public List<AntelopeDoc> search(String query, boolean loadContent) throws Exception {
        return search(query, 1, 20, loadContent);
    }

    public List<AntelopeDoc> search(String query, int page, int size, boolean needdata) throws Exception {
        if (page < 1) page = 1;
        int index = (page - 1) * size;

        long hits = server.search(query);

        if (index > 0)
            server.goto_result(index);

        // server.result_to_outchannel();
        AntelopeSearchResult searchResult = new AntelopeSearchResult(hits, page, size, server.get_search_time());
        List<AntelopeDoc> results = search(query, page, size);

        if (results != null && needdata)
            for (AntelopeDoc antelopeDoc : results)
                antelopeDoc.doc = loadDocument((int) antelopeDoc.docid);

        //logger.info({query: query, index: index, page_size: size, hits: hits, size: results.list.length});

        return results;
    }

    public String loadDocument(int docid) {
        return server.get_document(docid);
    }

    public String sendCommand(String cmd) {
        server.insert_command(cmd);
        server.process_command();

        return server.get_outchannel_content();
    }

    @Override
    public AntelopeSearchResult search(String query, int pageIndex, int pageSize) throws Exception {
        List list = null;

        int hits = server.search(query);

        if (pageIndex > 0)
            server.goto_result(pageIndex);

        AntelopeSearchResult searchResult = new AntelopeSearchResult(hits, pageIndex, pageSize, server.get_search_time());

        int ret = server.next_result();
        int count = 0;
        if (ret > 0) {
            list = new ArrayList();
            while (ret > 0 && count < pageSize) {

                // String hit = server.result_to_json();

                AntelopeDoc obj = new AntelopeDoc();
                ATIRE_API_result result = server.get_result();
                obj.rank = result.getRank();
                obj.rsv = result.getRsv();
                obj.docid = result.getDocid();
                obj.document_name = result.getDocument_name();
                obj.title = result.getTitle();
                obj.snippet = result.getSnippet();

//                if (null != parser) {
//                    try {
//                        obj = parser.parse(hit);
//                    } catch (Exception err) {
//                        obj = null;
//                    }
//                }

                if (searchResult.list == null)
                    searchResult.list = new ArrayList();
//                if (null != obj) {
                    searchResult.list.add(obj);
//                }
//                else
//                    searchResult.list.add(hit);

                ret = server.next_result();

                ++count;
            }
        }

        return list;
    }

    @Override
    protected String getDocumentInternal(long id) {
        return server.get_document((int) id);
    }
}

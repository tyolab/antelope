package au.com.tyo.antelope;

import java.util.ArrayList;
import java.util.List;

import au.com.tyo.antelope.jni.ATIRE_API_result;
import au.com.tyo.antelope.jni.ATIRE_API_server;

public abstract class Antelope<DocumentType extends AntelopeDoc> extends AntelopeClient<DocumentType> {

    private String opts;

    private ATIRE_API_server server;

    private AntelopeParser parser;

    private static boolean nativeLibraryLoaded = false;

    public static void loadNativeLibrary() {
        if (!nativeLibraryLoaded) {
            System.loadLibrary(dylibName);
            nativeLibraryLoaded = true;
        }
    }

    public Antelope(String opts) {
        this.opts = opts;
    }

    public void initializeServer() {
        server = new ATIRE_API_server();

        server.set_params(opts);
        server.set_outchannel(3);
        server.initialize();
    }

    public boolean isServerOnline() {
        return null != server;
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
        server = null;
    }

    public AntelopeSearchResult search(String query, boolean loadContent) throws Exception {
        return search(query, 1, 20, loadContent);
    }

    public AntelopeSearchResult search(String query, int page, int size, boolean needdata) throws Exception {
        if (page < 1) page = 1;
        int index = (page - 1) * size;

        if (index > 0)
            server.goto_result(index);

        AntelopeSearchResult<DocumentType> results = search(query, page, size);

        if (results != null && needdata)
            for (AntelopeDoc antelopeDoc : results.list)
                antelopeDoc.doc = loadDocument((int) antelopeDoc.docid);

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
    public AntelopeSearchResult<DocumentType> search(String query, int pageIndex, int pageSize) {

        int hits = server.search(query);

        if (pageIndex > 0)
            server.goto_result(pageIndex);

        AntelopeSearchResult<DocumentType> searchResult = new AntelopeSearchResult<DocumentType>(hits, pageIndex, pageSize, server.get_search_time());

        int ret = server.next_result();
        int count = 0;
        if (ret > 0) {
            while (ret > 0 && count < pageSize) {

                ATIRE_API_result result = server.get_result();
                DocumentType searchDoc = createNewSearchResult(result.getDocid(),
                        result.getTitle(),
                        result.getRank(),
                        result.getDocument_name(),
                        result.getSnippet(),
                        result.getRsv());


                if (searchResult.list == null)
                    searchResult.list = new ArrayList();

                searchResult.list.add(searchDoc);

                ret = server.next_result();

                ++count;
            }
        }

        return searchResult;
    }

    @Override
    protected String getDocumentInternal(long id) {
        return server.get_document((int) id);
    }

    protected DocumentType createNewSearchResult(long docId, String title, int rank) {
        return createNewSearchResult(docId, title, rank, null, null, 0);
    }

    protected DocumentType createNewSearchResult(long docId, String fileName, int rank, String documentName, String snippet, float rsv) {
        AntelopeDoc obj = new AntelopeDoc();

        obj.rank = rank;
        obj.rsv = rsv;
        obj.docid = docId;
        obj.document_name = documentName;
        obj.title = fileName;
        obj.snippet = snippet;

        return (DocumentType) obj;
    }

    @Override
    public List<DocumentType> listTerm(String query, int pageSize, int pageIndex) {
        List<DocumentType> list = null;

        ATIRE_API_result termResult = server.list_term(query, pageSize * pageIndex);

        if (null != termResult) {
            int count = 0;
            list = new ArrayList<>();
            list.add(createNewSearchResult(termResult.getDocid(), termResult.getTitle(), count));

            while ((termResult = server.next_term()) != null)
                list.add(createNewSearchResult(termResult.getDocid(), termResult.getTitle(), ++count));
        }
        return list;
    }
}

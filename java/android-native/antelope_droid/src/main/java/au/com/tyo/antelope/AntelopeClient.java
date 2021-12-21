package au.com.tyo.antelope;

import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.UnsupportedEncodingException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public abstract class AntelopeClient<DocumentType extends AntelopeDoc> {

    public static final int DEFAULT_PAGE_SIZE = 10;

    public static final String COMMAND_LIST_TERM = ".listterm";

    private int pageSize;

    protected long hits;

    protected ByteArrayKMP kmpSearch;

    protected byte[] buffer;


    public static String dylibName = "antelope_jni";

    private static AntelopeClient instance;

    private boolean needsConvertChinese;

    protected String preQuery;
    private int errorCode;

    public AntelopeClient() {

        kmpSearch = new ByteArrayKMP();
        pageSize = DEFAULT_PAGE_SIZE;

        init();
    }

    private void init() {
        this.needsConvertChinese = false;
        hits = 0;
        errorCode = 404;
    }

    public static AntelopeClient getInstance() {
        return instance;
    }

    public static void setInstance(AntelopeClient instance) {
        AntelopeClient.instance = instance;
    }

    public boolean isNeedsConvertChinese() {
        return needsConvertChinese;
    }

    public void setNeedsConvertChinese(boolean needsConvertChinese) {
        this.needsConvertChinese = needsConvertChinese;
    }

    /*
    BETWEEN()
    ---------
    written by Andrew Trotman
    */
    protected String between(String open_tag, String close_tag, int pos)	{
        String got = "";
        int start = pos, finish;
        int index = 0;
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

    protected String between(String open_tag, String close_tag) {
        return between(open_tag, close_tag, 0);
    }

    protected abstract String sendCommand(String command);

    public List<DocumentType> listTitle(String query) {
        return listTerm("tf:" + query);
    }

    /**
     * Listing term only return:
     *
     * <ATIREresult>
     *     docid#1
     *     docid#2
     *     docid#3
     *     ...
     * </ATIREresult>
     *
     * @param query
     * @return
     */
    public List<DocumentType> listTerm(String query) {
        return listTerm(query, 100, 0);
    }

    public List<DocumentType> listTerm(String query, int pageSize, int pageIndex) {
        return listTermByCommand(query, pageSize, pageIndex);
    }

    public List<DocumentType> listTermByCommand(String query, int pageSize, int pageIndex) {
        List<DocumentType> list = new ArrayList<>();

        String newQuery = query;
        String command;
        if (pageSize > 0) {
            command = COMMAND_LIST_TERM + ':' + pageSize;

            if (pageIndex > 0)
                command += ':' + pageIndex;
        }
        else
            command = COMMAND_LIST_TERM;

        String result = sendCommand(command + ' ' + newQuery);

        if (result != null && result.length() > 0) {
            InputStream is = new ByteArrayInputStream(result.getBytes());

            BufferedReader br = new BufferedReader(new InputStreamReader(is));

            String line;
            int count = 0;
            try {
                while ((line = br.readLine()) != null) {

                    if (count > 0 && line.length() > 0 && line.charAt(0) != '<') {

                        String[] tokens = line.split(":");
                        long docid;
                        String title;
                        if (tokens != null && tokens.length > 1) {
                            title = tokens[1];
                            try {
                                docid = Long.parseLong(tokens[0]);
                            } catch (Exception ex) {
                                docid = -1;
                            }

                            DocumentType document = createNewSearchResult(docid, title, list.size());
                            list.add(document);
                        }
//                        try {
//                            long docid = Long.parseLong(line);
//                            search = new AntelopeDoc();
//                            search.docid = docid;
//                        }
//                        catch (Exception ex) {}
//
//
//                        if (null != search) {
//                            search.rank = list.size();
//                            list.add(search);
//                        }
                    }
                    ++count;
                }

                br.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
        }

        if (list.size() == 0)
            try {
                return search(query).list;
            } catch (Exception e) {
                return list;
            }

        preQuery = query;
        return list;
    }

    protected DocumentType createNewSearchResult(long docId, String fileName, int rank) {
        AntelopeDoc search = new AntelopeDoc();

        search.docid = docId;
        search.title = fileName;
        search.rank = rank;

        return (DocumentType) search;
    }

    public AntelopeSearchResult<DocumentType> search(String query) throws Exception {
        return search(query, 0, 10);
    }

    public abstract AntelopeSearchResult<DocumentType> search(String query, int pageIndex, int pageSize);

    protected String buildSearchQueries(String query) {
        StringBuffer buffer = new StringBuffer();
        String[] tokens = query.split("\\s");
        if (tokens != null)
            for (int i = 0; i < tokens.length; ++i) {
                String term = tokens[i];

                ArrayList<String> list = new ArrayList<String>(); //ChineseUtils.charToList(term);
                if (list.size() > 0) {
                    for (int j = 0; j < list.size(); ++j) {
                        buffer.append("t:" + list.get(j) + " ");
                        buffer.append(list.get(j) + " ");
                    }
                }
            }
        else {
            buffer.append("tf:" + query + " ");
            buffer.append(query);
        }
        return buffer.toString();
    }

//    public long getDocumentId(String name) {
//        AntelopeDoc search = results.get(name);
//        if (search != null && search.docid > -1)
//            return search.docid;
//        return -1;
//    }
//
//    public String getDocumentAbstract(String name) {
//        AntelopeDoc search = results.get(name);
//        if (search != null && search.docid > -1)
//            return getDocumentAbstractById(search.docid);
//        return "";
//    }

    public String getDocument(long id) {
        return getDocumentInternal(id);
    }

    protected abstract String getDocumentInternal(long id);

    public String getDocumentAbstractById(long id) {

        String abs  = "";

        String result = getDocument(id);

        byte[] bytes = result.getBytes();

        ByteArrayKMP kmp1 = new ByteArrayKMP("<ABSTRACT>".getBytes());
        ByteArrayKMP kmp2 = new ByteArrayKMP("</ABSTRACT>".getBytes());

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
        return abs;
    }

    public int getErrorCode() {
        return errorCode;
    }
}

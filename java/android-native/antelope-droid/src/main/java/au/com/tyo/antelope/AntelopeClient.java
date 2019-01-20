package au.com.tyo.antelope;

import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.UnsupportedEncodingException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;

public abstract class AntelopeClient {

    public static final int DEFAULT_PAGE_SIZE = 10;

    private int pageSize;

    protected HashMap<String, AntelopeDoc> results;

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

        results = new HashMap<String, AntelopeDoc>();

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

    public List<AntelopeDoc> listTitle(String query) {
        return listTerm("tf:" + query);
    }

    public List<AntelopeDoc> listTerm(String query) {
        List<AntelopeDoc> list = new ArrayList<>();

        String newQuery = query;

        results.clear();

            String result = sendCommand(".listterm " + newQuery);

            if (result != null && result.length() > 0) {
                InputStream is = new ByteArrayInputStream(result.getBytes());

                BufferedReader br = new BufferedReader(new InputStreamReader(is));

                String line;
                int count = 0;
                try {
                    while ((line = br.readLine()) != null && line.length() > 3) {
                        String[] tokens = line.substring(3).split(":");
                        if (tokens != null && tokens.length > 1) {
                            // list.add(tokens[0]);

                            AntelopeDoc search = new AntelopeDoc();
                            search.title = tokens[0];
                            try {
                                search.docid = Long.parseLong(tokens[tokens.length - 1]);
                            }
                            catch (Exception ex) {
                                search.docid = -1;
                            }
                            search.rank = ++count;

                            list.add(search);
                            results.put(tokens[0], search);
                        }
                    }

                    br.close();
                } catch (IOException e) {
                    e.printStackTrace();
                }
            }

        if (list.size() == 0)
            try {
                return search(query);
            } catch (Exception e) {
                return list;
            }

        preQuery = query;
        return list;
    }

    public List<AntelopeDoc> search(String query) throws Exception {
        return search(query, 1, 10);
    }

    public abstract List<AntelopeDoc> search(String query, int pageIndex, int pageSize) throws Exception;

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

    public long getDocumentId(String name) {
        AntelopeDoc search = results.get(name);
        if (search != null && search.docid > -1)
            return search.docid;
        return -1;
    }

    public String getDocumentAbstract(String name) {
        AntelopeDoc search = results.get(name);
        if (search != null && search.docid > -1)
            return getDocumentAbstractById(search.docid);
        return "";
    }

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

    public static void main(String[] args) {
        AntelopeClientRemote.dylibName = "antelope_jni";
        System.loadLibrary(dylibName);

        AntelopeClientRemote atire = new AntelopeClientRemote();

        List<AntelopeDoc> results = null;

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
                        catch (Exception connEx) {
                            System.err.println(connEx.getMessage());

                            atire.initializeSocket();

                            try {
                                results = atire.search(fromUser);
                            }
                            catch (Exception connEx2) {
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
            } catch (Exception e) {
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

    public void clearResults() {
        results.clear();
    }

    public int getErrorCode() {
        return errorCode;
    }
}

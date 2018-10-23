package au.com.tyo.antelope;

public class AntelopeDoc {

    public long docid;
    public int rank;

    float rsv;

    public String title;

    String snippet;

    public String document_name;

    String doc;

    public String toString() {
        return "rank: " + rank + ", " + "name: " + document_name + ", " + "id: " + docid + ", " + "rsv: " + rsv;
    }

    public long getDocid() {
        return docid;
    }

    public int getRank() {
        return rank;
    }

    public float getRsv() {
        return rsv;
    }

    public String getTitle() {
        return title;
    }

    public String getSnippet() {
        return snippet;
    }

    public String getDocument_name() {
        return document_name;
    }

    public String getDoc() {
        return doc;
    }
}

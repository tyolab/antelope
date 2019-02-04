package au.com.tyo.antelope;

public class AntelopeDoc {

    public long docid = -1;

    public int rank;

    public float rsv;

    public String title;

    public String snippet;

    public String document_name;

    /**
     * The content of the retrieved document
     */
    public String doc;

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

    public String getDocumentName() {
        return document_name;
    }

    public String getDoc() {
        return doc;
    }

    /**
     *
     * @param doc
     */
    public void setDoc(String doc) {
        this.doc = doc;
    }
}

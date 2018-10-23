package au.com.tyo.antelope;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

public class Utils {

    /**
     * 	// In android,
     // We guarantee that the available method returns the total
     // size of the asset...  of course, this does mean that a single
     // asset can't be more than 2 gigs.
     *
     * @param is
     * @return
     * @throws IOException
     */
    static public byte[] readFileIntoBytes(InputStream is) throws IOException {
        int size;
        byte[] bytes = null;

        size = is.available();
        bytes = new byte[size];
        is.read(bytes, 0, size);
        return bytes;
    }

    /**
     *
     * @param file
     * @return
     */
    static public byte[] readFileIntoBytes(File file) throws IOException {
        byte[] bytes = null;
        FileInputStream fis = null;
        try {
            fis = new FileInputStream(file);
            bytes = readFileIntoBytes(fis);
        } catch (IOException e) {
            throw e;
        }
        finally {
            if (null != fis)
                try {
                    fis.close();
                } catch (IOException e) {
                    e.printStackTrace();
                }
        }
        return bytes;
    }

    /**
     * TODO
     * charsets
     */
    static public byte[] readFileIntoBytes(String file) throws IOException {
        return readFileIntoBytes(new File(file));
    }
}

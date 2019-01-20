package au.com.tyo.antelope;

import org.junit.Test;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;

import static org.junit.Assert.assertEquals;

/**
 * Example local unit test, which will execute on the development machine (host).
 *
 * @see <a href="http://d.android.com/tools/testing">Testing documentation</a>
 */
public class MainUnitTest {
    @Test
    public void addition_isCorrect() {
        assertEquals(4, 2 + 2);
    }

    public static void main(String[] args) {
        AntelopeClientRemote.dylibName = "antelope_jni";
        System.loadLibrary(AntelopeClientRemote.dylibName);

        AntelopeClientRemote atire = new AntelopeClientRemote();

        AntelopeSearchResult results = null;

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
            for (int i = 0; i < results.list.size(); ++i)
                System.out.println(results.list.get(i));
            System.out.println("");
        }
    }
}
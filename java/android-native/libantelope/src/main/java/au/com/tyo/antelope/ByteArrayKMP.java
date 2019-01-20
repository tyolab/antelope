/*
 * Copyright (C) 2015 TYONLINE TECHNOLOGY PTY. LTD. (TYO Lab)
 * 
 */

package au.com.tyo.antelope;

/**
 * Knuth-Morris-Pratt agent implementation for looking for bytes in a byte array.
 * <br>Based on<pre>
 * Algorthms in C
 * Robert Sedgewick
 * Addison Wesley
 * ISBN 0-201-514254-7</pre>
 *
 * @author Sabre
 * @see <a href="http://en.wikipedia.org/wiki/Knuth-Morris-Pratt_algorithm" target="right">Wikipedia</a>
 * <a href="http://www.ics.uci.edu/~eppstein/161/960227.html" target="right">Eppstein</a>
 */
public class ByteArrayKMP
{
	
    /**
     * Constructor
     */
    public ByteArrayKMP() {
	}
    
    /**
     * Contructs the KMP agent to look for the given byte[] pattern.
     *
     * @param pattern the pattern to search for.
     */
    public ByteArrayKMP(byte[] pattern)
    {
    	setPattern(pattern);
    }

	public void setPattern(byte[] pattern)
    {
        pattern_ = (byte[])pattern.clone();
        next_ = new int[pattern_.length];
        next_[0] = -1;
        for (int len = next_.length-1, i = 0, j = -1; i < len; next_[++i] = ++j)
        {
            while((j >= 0) && (pattern_[i] != pattern_[j]))
            {
                j = next_[j];
            }
        }
    }
    
    /**
     * Searches a range of byte from a start index
     * in a byte array for the pattern used in the construction
     * of this agent. Returns the index of the start of the match or -1
     * if the pattern is not found.
     *
     * @param dataToSearch the data to search.
     * @param start the start point in the data
     * @param length the number of byte to search.
     * @return the index of the start of the match or -1 if not found.
     */
    public int search(byte[] dataToSearch, int start, int length)
    {
    	if (dataToSearch == null || dataToSearch.length == 0)
    		return -1;
    	
        final int end = start + length;
        if (end > dataToSearch.length)
            throw new IllegalArgumentException("Invalid range");
        
        int i = start;
        int j = 0;
        for (; (j < next_.length) && (i < end); i++, j++)
        {
            while ((j >= 0) && (dataToSearch[i] != pattern_[j]))
            {
                j = next_[j];
            }
        }
        return (j == next_.length) ? i - next_.length : -1;
    }
    
    /**
     * Searches from a start index until the end of the data
     * in a byte array for the pattern used in the construction
     * of this agent. Returns the index of the start of the match or -1
     * if the pattern is not found.
     *
     * @param dataToSearch the data to search.
     * @param start the start point in the data
     * @return the index of the start of the match or -1 if not found.
     */
    public int search(byte[] dataToSearch, int start)
    {
        return search(dataToSearch, start, dataToSearch.length - start);
    }
    
    /**
     * Searches from the start until the end of the data
     * in a byte array for the pattern used in the construction
     * of this agent. Returns the index of the start of the match or -1
     * if the pattern is not found.
     *
     * @param dataToSearch the data to search.
     * @return the index of the start of the match or -1 if not found.
     */
    public int search(byte[] dataToSearch)
    {
        return search(dataToSearch, 0);
    }
    
    private byte[] pattern_;
    private int[] next_;
}

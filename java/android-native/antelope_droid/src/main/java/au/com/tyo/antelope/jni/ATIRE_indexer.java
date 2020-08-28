/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 4.1.0
 *
 * Do not make changes to this file unless you know what you are doing--modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

package au.com.tyo.antelope.jni;

public class ATIRE_indexer {
  private transient long swigCPtr;
  protected transient boolean swigCMemOwn;

  protected ATIRE_indexer(long cPtr, boolean cMemoryOwn) {
    swigCMemOwn = cMemoryOwn;
    swigCPtr = cPtr;
  }

  protected static long getCPtr(ATIRE_indexer obj) {
    return (obj == null) ? 0 : obj.swigCPtr;
  }

  @SuppressWarnings("deprecation")
  protected void finalize() {
    delete();
  }

  public synchronized void delete() {
    if (swigCPtr != 0) {
      if (swigCMemOwn) {
        swigCMemOwn = false;
        AntelopeJNI.delete_ATIRE_indexer(swigCPtr);
      }
      swigCPtr = 0;
    }
  }

  public static void setEMPTY_DOCUMENT_CONTENT(String value) {
    AntelopeJNI.ATIRE_indexer_EMPTY_DOCUMENT_CONTENT_set(value);
  }

  public static String getEMPTY_DOCUMENT_CONTENT() {
    return AntelopeJNI.ATIRE_indexer_EMPTY_DOCUMENT_CONTENT_get();
  }

  public static void setEMPTY_DOCUMENT_FILENAME(String value) {
    AntelopeJNI.ATIRE_indexer_EMPTY_DOCUMENT_FILENAME_set(value);
  }

  public static String getEMPTY_DOCUMENT_FILENAME() {
    return AntelopeJNI.ATIRE_indexer_EMPTY_DOCUMENT_FILENAME_get();
  }

  public static int getEMPTY_DOUCMENT_LENGTH() {
    return AntelopeJNI.ATIRE_indexer_EMPTY_DOUCMENT_LENGTH_get();
  }

  public ATIRE_indexer() {
    this(AntelopeJNI.new_ATIRE_indexer(), true);
  }

  public void usage() {
    AntelopeJNI.ATIRE_indexer_usage(swigCPtr, this);
  }

  public void init(String options) {
    AntelopeJNI.ATIRE_indexer_init__SWIG_0(swigCPtr, this, options);
  }

  public void init(int argc, SWIGTYPE_p_p_char argv) {
    AntelopeJNI.ATIRE_indexer_init__SWIG_1(swigCPtr, this, argc, SWIGTYPE_p_p_char.getCPtr(argv));
  }

  public int finish() {
    return AntelopeJNI.ATIRE_indexer_finish(swigCPtr, this);
  }

  public long index_document(SWIGTYPE_p_ANT_directory_iterator_object current_file, String doc_to_store) {
    return AntelopeJNI.ATIRE_indexer_index_document__SWIG_0(swigCPtr, this, SWIGTYPE_p_ANT_directory_iterator_object.getCPtr(current_file), doc_to_store);
  }

  public long index_document(SWIGTYPE_p_ANT_directory_iterator_object current_file) {
    return AntelopeJNI.ATIRE_indexer_index_document__SWIG_1(swigCPtr, this, SWIGTYPE_p_ANT_directory_iterator_object.getCPtr(current_file));
  }

  public void index_document(String file_name, String file, String doc_to_store) {
    AntelopeJNI.ATIRE_indexer_index_document__SWIG_2(swigCPtr, this, file_name, file, doc_to_store);
  }

  public void index_document(String file_name, String file) {
    AntelopeJNI.ATIRE_indexer_index_document__SWIG_3(swigCPtr, this, file_name, file);
  }

  public void index() {
    AntelopeJNI.ATIRE_indexer_index(swigCPtr, this);
  }

  public void enable_parallel_indexing() {
    AntelopeJNI.ATIRE_indexer_enable_parallel_indexing(swigCPtr, this);
  }

  public static boolean initialize() {
    return AntelopeJNI.ATIRE_indexer_initialize();
  }

}

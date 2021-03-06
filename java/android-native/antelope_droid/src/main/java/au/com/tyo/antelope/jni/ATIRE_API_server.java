/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 4.1.0
 *
 * Do not make changes to this file unless you know what you are doing--modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

package au.com.tyo.antelope.jni;

public class ATIRE_API_server {
  private transient long swigCPtr;
  protected transient boolean swigCMemOwn;

  protected ATIRE_API_server(long cPtr, boolean cMemoryOwn) {
    swigCMemOwn = cMemoryOwn;
    swigCPtr = cPtr;
  }

  protected static long getCPtr(ATIRE_API_server obj) {
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
        AntelopeJNI.delete_ATIRE_API_server(swigCPtr);
      }
      swigCPtr = 0;
    }
  }

  public ATIRE_API_server() {
    this(AntelopeJNI.new_ATIRE_API_server(), true);
  }

  public void initialize() {
    AntelopeJNI.ATIRE_API_server_initialize(swigCPtr, this);
  }

  public SWIGTYPE_p_ATIRE_API get_atire() {
    long cPtr = AntelopeJNI.ATIRE_API_server_get_atire(swigCPtr, this);
    return (cPtr == 0) ? null : new SWIGTYPE_p_ATIRE_API(cPtr, false);
  }

  public void set_params(int argc, SWIGTYPE_p_p_char argv) {
    AntelopeJNI.ATIRE_API_server_set_params__SWIG_0(swigCPtr, this, argc, SWIGTYPE_p_p_char.getCPtr(argv));
  }

  public void set_params(String args) {
    AntelopeJNI.ATIRE_API_server_set_params__SWIG_1(swigCPtr, this, args);
  }

  public void set_param(int param) {
    AntelopeJNI.ATIRE_API_server_set_param(swigCPtr, this, param);
  }

  public void set_ant_version(int version) {
    AntelopeJNI.ATIRE_API_server_set_ant_version(swigCPtr, this, version);
  }

  public void start() {
    AntelopeJNI.ATIRE_API_server_start(swigCPtr, this);
  }

  public void loop() {
    AntelopeJNI.ATIRE_API_server_loop(swigCPtr, this);
  }

  public void poll() {
    AntelopeJNI.ATIRE_API_server_poll(swigCPtr, this);
  }

  public void poll_and_process() {
    AntelopeJNI.ATIRE_API_server_poll_and_process(swigCPtr, this);
  }

  public void process_command() {
    AntelopeJNI.ATIRE_API_server_process_command(swigCPtr, this);
  }

  public void interrupt() {
    AntelopeJNI.ATIRE_API_server_interrupt(swigCPtr, this);
  }

  public void finish() {
    AntelopeJNI.ATIRE_API_server_finish(swigCPtr, this);
  }

  public int run() {
    return AntelopeJNI.ATIRE_API_server_run__SWIG_0(swigCPtr, this);
  }

  public int run(String files) {
    return AntelopeJNI.ATIRE_API_server_run__SWIG_1(swigCPtr, this, files);
  }

  public void ant() {
    AntelopeJNI.ATIRE_API_server_ant(swigCPtr, this);
  }

  public void start_stats() {
    AntelopeJNI.ATIRE_API_server_start_stats(swigCPtr, this);
  }

  public void end_stats() {
    AntelopeJNI.ATIRE_API_server_end_stats(swigCPtr, this);
  }

  public SWIGTYPE_p_ANT_stats get_stats() {
    long cPtr = AntelopeJNI.ATIRE_API_server_get_stats(swigCPtr, this);
    return (cPtr == 0) ? null : new SWIGTYPE_p_ANT_stats(cPtr, false);
  }

  public long get_search_time() {
    return AntelopeJNI.ATIRE_API_server_get_search_time(swigCPtr, this);
  }

  public String version() {
    return AntelopeJNI.ATIRE_API_server_version(swigCPtr, this);
  }

  public void prompt() {
    AntelopeJNI.ATIRE_API_server_prompt(swigCPtr, this);
  }

  public int is_interrupted() {
    return AntelopeJNI.ATIRE_API_server_is_interrupted(swigCPtr, this);
  }

  public void set_interrupted(int interrupted) {
    AntelopeJNI.ATIRE_API_server_set_interrupted(swigCPtr, this, interrupted);
  }

  public int has_new_command() {
    return AntelopeJNI.ATIRE_API_server_has_new_command(swigCPtr, this);
  }

  public void insert_command(String cmd) {
    AntelopeJNI.ATIRE_API_server_insert_command(swigCPtr, this, cmd);
  }

  public void set_outchannel(int type) {
    AntelopeJNI.ATIRE_API_server_set_outchannel(swigCPtr, this, type);
  }

  public String get_outchannel_content() {
    return AntelopeJNI.ATIRE_API_server_get_outchannel_content(swigCPtr, this);
  }

  public int search(String query) {
    return AntelopeJNI.ATIRE_API_server_search__SWIG_0(swigCPtr, this, query);
  }

  public int search() {
    return AntelopeJNI.ATIRE_API_server_search__SWIG_1(swigCPtr, this);
  }

  public void goto_result(int index) {
    AntelopeJNI.ATIRE_API_server_goto_result(swigCPtr, this, index);
  }

  public ATIRE_API_result list_term(String term, int position) {
    long cPtr = AntelopeJNI.ATIRE_API_server_list_term__SWIG_0(swigCPtr, this, term, position);
    return (cPtr == 0) ? null : new ATIRE_API_result(cPtr, false);
  }

  public ATIRE_API_result list_term(String term) {
    long cPtr = AntelopeJNI.ATIRE_API_server_list_term__SWIG_1(swigCPtr, this, term);
    return (cPtr == 0) ? null : new ATIRE_API_result(cPtr, false);
  }

  public ATIRE_API_result next_term(int to_position) {
    long cPtr = AntelopeJNI.ATIRE_API_server_next_term__SWIG_0(swigCPtr, this, to_position);
    return (cPtr == 0) ? null : new ATIRE_API_result(cPtr, false);
  }

  public ATIRE_API_result next_term() {
    long cPtr = AntelopeJNI.ATIRE_API_server_next_term__SWIG_1(swigCPtr, this);
    return (cPtr == 0) ? null : new ATIRE_API_result(cPtr, false);
  }

  public ATIRE_API_result goto_term(int index) {
    long cPtr = AntelopeJNI.ATIRE_API_server_goto_term(swigCPtr, this, index);
    return (cPtr == 0) ? null : new ATIRE_API_result(cPtr, false);
  }

  public String result_to_json() {
    return AntelopeJNI.ATIRE_API_server_result_to_json(swigCPtr, this);
  }

  public ATIRE_API_result get_result() {
    long cPtr = AntelopeJNI.ATIRE_API_server_get_result(swigCPtr, this);
    return (cPtr == 0) ? null : new ATIRE_API_result(cPtr, false);
  }

  public int next_result() {
    return AntelopeJNI.ATIRE_API_server_next_result(swigCPtr, this);
  }

  public void result_to_outchannel(int last_to) {
    AntelopeJNI.ATIRE_API_server_result_to_outchannel__SWIG_0(swigCPtr, this, last_to);
  }

  public void result_to_outchannel() {
    AntelopeJNI.ATIRE_API_server_result_to_outchannel__SWIG_1(swigCPtr, this);
  }

  public String load_document() {
    return AntelopeJNI.ATIRE_API_server_load_document(swigCPtr, this);
  }

  public String get_document(int docid) {
    return AntelopeJNI.ATIRE_API_server_get_document(swigCPtr, this, docid);
  }

  public String get_current_document() {
    return AntelopeJNI.ATIRE_API_server_get_current_document(swigCPtr, this);
  }

  public long get_document_count() {
    return AntelopeJNI.ATIRE_API_server_get_document_count(swigCPtr, this);
  }

  public void set_page_size(int page_size) {
    AntelopeJNI.ATIRE_API_server_set_page_size(swigCPtr, this, page_size);
  }

  public void set_output_format(int format) {
    AntelopeJNI.ATIRE_API_server_set_output_format(swigCPtr, this, format);
  }

  public final static class ATIRE_channel_type {
    public final static ATIRE_API_server.ATIRE_channel_type CHANNEL_FILE = new ATIRE_API_server.ATIRE_channel_type("CHANNEL_FILE", AntelopeJNI.ATIRE_API_server_CHANNEL_FILE_get());
    public final static ATIRE_API_server.ATIRE_channel_type CHANNEL_SOCKET = new ATIRE_API_server.ATIRE_channel_type("CHANNEL_SOCKET", AntelopeJNI.ATIRE_API_server_CHANNEL_SOCKET_get());
    public final static ATIRE_API_server.ATIRE_channel_type CHANNEL_MEMORY = new ATIRE_API_server.ATIRE_channel_type("CHANNEL_MEMORY", AntelopeJNI.ATIRE_API_server_CHANNEL_MEMORY_get());

    public final int swigValue() {
      return swigValue;
    }

    public String toString() {
      return swigName;
    }

    public static ATIRE_channel_type swigToEnum(int swigValue) {
      if (swigValue < swigValues.length && swigValue >= 0 && swigValues[swigValue].swigValue == swigValue)
        return swigValues[swigValue];
      for (int i = 0; i < swigValues.length; i++)
        if (swigValues[i].swigValue == swigValue)
          return swigValues[i];
      throw new IllegalArgumentException("No enum " + ATIRE_channel_type.class + " with value " + swigValue);
    }

    private ATIRE_channel_type(String swigName) {
      this.swigName = swigName;
      this.swigValue = swigNext++;
    }

    private ATIRE_channel_type(String swigName, int swigValue) {
      this.swigName = swigName;
      this.swigValue = swigValue;
      swigNext = swigValue+1;
    }

    private ATIRE_channel_type(String swigName, ATIRE_channel_type swigEnum) {
      this.swigName = swigName;
      this.swigValue = swigEnum.swigValue;
      swigNext = this.swigValue+1;
    }

    private static ATIRE_channel_type[] swigValues = { CHANNEL_FILE, CHANNEL_SOCKET, CHANNEL_MEMORY };
    private static int swigNext = 0;
    private final int swigValue;
    private final String swigName;
  }

  public final static class ATIRE_output_format {
    public final static ATIRE_API_server.ATIRE_output_format XML = new ATIRE_API_server.ATIRE_output_format("XML", AntelopeJNI.ATIRE_API_server_XML_get());
    public final static ATIRE_API_server.ATIRE_output_format JSON = new ATIRE_API_server.ATIRE_output_format("JSON", AntelopeJNI.ATIRE_API_server_JSON_get());

    public final int swigValue() {
      return swigValue;
    }

    public String toString() {
      return swigName;
    }

    public static ATIRE_output_format swigToEnum(int swigValue) {
      if (swigValue < swigValues.length && swigValue >= 0 && swigValues[swigValue].swigValue == swigValue)
        return swigValues[swigValue];
      for (int i = 0; i < swigValues.length; i++)
        if (swigValues[i].swigValue == swigValue)
          return swigValues[i];
      throw new IllegalArgumentException("No enum " + ATIRE_output_format.class + " with value " + swigValue);
    }

    private ATIRE_output_format(String swigName) {
      this.swigName = swigName;
      this.swigValue = swigNext++;
    }

    private ATIRE_output_format(String swigName, int swigValue) {
      this.swigName = swigName;
      this.swigValue = swigValue;
      swigNext = swigValue+1;
    }

    private ATIRE_output_format(String swigName, ATIRE_output_format swigEnum) {
      this.swigName = swigName;
      this.swigValue = swigEnum.swigValue;
      swigNext = this.swigValue+1;
    }

    private static ATIRE_output_format[] swigValues = { XML, JSON };
    private static int swigNext = 0;
    private final int swigValue;
    private final String swigName;
  }

}

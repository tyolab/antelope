/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 4.1.0
 *
 * Do not make changes to this file unless you know what you are doing--modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

package au.com.tyo.antelope.jni;

public class Antelope implements AntelopeConstants {
  public static void setANT_error_code(int value) {
    AntelopeJNI.ANT_error_code_set(value);
  }

  public static int getANT_error_code() {
    return AntelopeJNI.ANT_error_code_get();
  }

  public static void ANT_on_error(String error_info, int error_code) {
    AntelopeJNI.ANT_on_error__SWIG_0(error_info, error_code);
  }

  public static void ANT_on_error(String error_info) {
    AntelopeJNI.ANT_on_error__SWIG_1(error_info);
  }

}

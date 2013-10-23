package org.atire;

import org.atire.swig.atire_apis;

public class Atire {

	/**
	 * @param args
	 */
	public static void main(String[] args) {
		System.loadLibrary("atire_jni");
		
		if (args.length == 0) {
			atire_apis.run_atire("-q:");
		}
		atire_apis.run_atire(args[0]);
	}
}

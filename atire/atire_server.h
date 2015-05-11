/*
	ATIRE_SERVER.H
	--------------
*/

#ifndef ATIRE_ATIRE_SERVER_H_
#define ATIRE_ATIRE_SERVER_H_

/*
 * the atire server class
 *
 * by default it use the socket non-blocking mode (select())
 * also, it
 */

class ANT_atire_server
{
private:
	long stop_signal; // 0 keep running, 1 time to stop

public:
	ANT_atire_server(int port = 8088);
	virtual ~ANT_atire_server();

	void start();

	void set_stop_signal() { stop_signal = 1; }
};

#endif /* ATIRE_ATIRE_SERVER_H_ */

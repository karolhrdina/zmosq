/*  =========================================================================
    zmosq_server - Zmosq actor

    Copyright (c) the Contributors as noted in the AUTHORS file.       
    This file is part of the Malamute Project.                         
                                                                       
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.           
    =========================================================================
*/

/*
@header
    zmosq_server - Zmosq actor forwarding MQTT messages to zeromq
@discuss
zmosq connects to remote Mosquito server, subscribes for topics and make them
available as zeromq messages on the inproc pipe. It spawns and stops mosquitto
client thread and provides standard interface on how to read data from MQTT.

Each zeromq message has two frames [MQTT topic|MQTT payload]
@end
*/

#include "zmsq_classes.h"

typedef struct mosquitto mosquitto_t;

//  Structure of our actor
struct _zmosq_server_t {
    zsock_t *pipe;              //  Actor command pipe
    bool terminated;            //  Did caller ask us to quit?
    bool verbose;               //  Verbose logging enabled?

    zuuid_t *uuid;              //  uuid, used for generating unique (inproc) endpoint
    zsock_t *mqtt_reader;
    zsock_t *mqtt_writter;
    zpoller_t *poller;          //  Socket poller

                                //  mosquitto:
    mosquitto_t *mosq;          //      client structure 
    char *host;                 //      hostname or ip of the broker to connect to
    int port;                   //      port
    int keepalive;              //      keepalive in seconds
    char *bind_address;         //      hostname or ip of local network interface to bind to
    zlistx_t *topics;           //      MQQT topics to subscribe to
};


//  --------------------------------------------------------------------------
//  Create a new zmosq_server instance

static void zmosq_server_destroy (zmosq_server_t **self_p);

static zmosq_server_t *
zmosq_server_new (zsock_t *pipe, void *args)
{
    zmosq_server_t *self = (zmosq_server_t *) zmalloc (sizeof (zmosq_server_t));
    if (!self)
        return NULL;

    //  pipe, terminated, verbose
    self->pipe = pipe;
    self->terminated = false;
    self->verbose = false;

    //  uuid
    self->uuid = zuuid_new ();
    if (!self->uuid) {
        zmosq_server_destroy (&self);
        return NULL;
    }

    //  mqtt_reader, mqtt_writter
    char *endpoint = zsys_sprintf (" inproc://%s-mqtt", zuuid_str_canonical (self->uuid));
    if (!endpoint) {
        zmosq_server_destroy (&self);
        return NULL;
    }

    endpoint [0] = '@';
    self->mqtt_writter = zsock_new_pair (endpoint);
    endpoint [0] = '>';
    self->mqtt_reader  = zsock_new_pair (endpoint);
    zstr_free (&endpoint);
    if (!self->mqtt_writter || !self->mqtt_reader) {
        zmosq_server_destroy (&self);
        return NULL;
    }
    
    //  poller
    self->poller = zpoller_new (self->pipe, self->mqtt_reader, NULL);
    if (!self->poller) {
        zmosq_server_destroy (&self);
        return NULL;
    }

    //  mosq + related
    self->mosq = mosquitto_new (
        zuuid_str_canonical (self->uuid),
        false,
        self);
    if (!self->mosq) {
        zmosq_server_destroy (&self);
        return NULL;
    }

    self->host = strdup ("");
    self->port = -1;
    self->keepalive = -1;
    self->bind_address = strdup ("");
    if (!self->host || !self->bind_address) {
        zmosq_server_destroy (&self);
        return NULL;
    }

    self->topics = zlistx_new ();
    if (!self->topics) {
        zmosq_server_destroy (&self);
        return NULL;
    }
    zlistx_set_duplicator (self->topics, (czmq_duplicator *) strdup);
    zlistx_set_destructor (self->topics, (czmq_destructor *) zstr_free);
    zlistx_set_comparator (self->topics, (czmq_comparator *) strcmp);

    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the zmosq_server instance

static void
zmosq_server_destroy (zmosq_server_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        zmosq_server_t *self = *self_p;

        zuuid_destroy (&self->uuid);
        zsock_destroy (&self->mqtt_writter);
        zsock_destroy (&self->mqtt_reader);
        zpoller_destroy (&self->poller);
        if (self->mosq) {
            mosquitto_destroy (self->mosq);
            self->mosq = NULL;
        }
        zstr_free (&self->host);
        zstr_free (&self->bind_address);
        zlistx_destroy (&self->topics);
        free (self);
        *self_p = NULL;
    }
}

static void
s_mosquitto_init ()
{
    static bool initialized = false;
    if (!initialized) {
        int r = mosquitto_lib_init ();
        if (r != MOSQ_ERR_SUCCESS) {
            zsys_error ("Cannot initialize mosquitto library: %s", mosquitto_strerror (r));
            exit (EXIT_FAILURE);
        }
        initialized = true;
    }
}

//  Start this actor. Return a value greater or equal to zero if initialization
//  was successful. Otherwise -1.

static int
zmosq_server_start (zmosq_server_t *self)
{
    assert (self);
    assert (self->mosq);

    mosquitto_loop_start (self->mosq);
    int r;
    r = mosquitto_connect_bind_async (
        self->mosq,
        self->host,
        self->port,
        self->keepalive,
        self->bind_address);

    if (r != MOSQ_ERR_SUCCESS) {
        zsys_error ("Can't connect to mosquito endpoint, run START again");
        mosquitto_loop_stop (self->mosq, true);
    }

    return 0;
}


//  Stop this actor. Return a value greater or equal to zero if stopping 
//  was successful. Otherwise -1.

static int
zmosq_server_stop (zmosq_server_t *self)
{
    assert (self);
    assert (self->mosq);

    //  TODO: Add shutdown actions
    mosquitto_loop_stop (self->mosq, true);
    mosquitto_disconnect (self->mosq);

    return 0;
}

//  Here we handle incoming message from the node
static void
zmosq_server_recv_api (zmosq_server_t *self)
{
    //  Get the whole message of the pipe in one go
    zmsg_t *request = zmsg_recv (self->pipe);
    if (!request)
       return;        //  Interrupted

    char *command = zmsg_popstr (request);
    if (streq (command, "START"))
        zmosq_server_start (self);
    else
    if (streq (command, "STOP")) {
        zmosq_server_stop (self);
    }
    else
    if (streq (command, "VERBOSE"))
        self->verbose = true;
    else
    if (streq (command, "CONNECT")) {
        zstr_free (&self->host);
        self->host = zmsg_popstr (request);
        assert (self->host);

        char *foo = zmsg_popstr (request);
        self->port = atoi (foo);
        zstr_free (&foo);

        foo = zmsg_popstr (request);
        self->keepalive = atoi (foo);
        if (self->keepalive <= 3)
            self->keepalive = 3;
        zstr_free (&foo);

        zstr_free (&self->bind_address);
        self->bind_address = zmsg_popstr (request);
        if (!self->bind_address)
            self->bind_address = strdup (self->host);
    }
    else
    if (streq (command, "SUBSCRIBE")) {
        char *topic = zmsg_popstr (request);
        while (topic) {
            void *handle = zlistx_find (self->topics, (void *) topic);
            if (!handle)
                zlistx_add_end (self->topics, topic);
            zstr_free (&topic);
            topic = zmsg_popstr (request);
        }
    }
    else
    if (streq (command, "PUBLISH")) {
        char *topic = zmsg_popstr (request);
        char *qosa = zmsg_popstr (request);
        char *retaina = zmsg_popstr (request);
        zframe_t *payload = zmsg_pop (request);

        int qos = 0;
        switch (qosa [0]) {
            case '1' : qos = 1; break;
            case '2' : qos = 2; break;
        }
        zstr_free (&qosa);

        bool retain = streq (retaina, "true");
        zstr_free (&retaina);

        int r = mosquitto_publish (
            self->mosq,
            NULL,
            topic,
            zframe_size (payload),
            zframe_data (payload),
            qos,
            retain);
        if (r != MOSQ_ERR_SUCCESS)
            zsys_warning ("Message on topic %s not published: %s", topic, mosquitto_strerror (r));
        zframe_destroy (&payload);
        zstr_free (&topic);
    }
    else
    if (streq (command, "$TERM")) {
        //  The $TERM command is send by zactor_destroy() method
        self->terminated = true;
        zmosq_server_stop (self);
    }
    else {
        zsys_error ("invalid command '%s'", command);
        assert (false);
    }
    zstr_free (&command);
    zmsg_destroy (&request);
}

static void
s_connect (struct mosquitto *mosq, void *obj, int result) {
    assert (obj);
    zmosq_server_t *self = (zmosq_server_t *) obj;

    if (!result) {
        char *topic = (char *) zlistx_first (self->topics);
        while (topic) {
            int r = mosquitto_subscribe (mosq, NULL, topic, 0);
            assert (r == MOSQ_ERR_SUCCESS);
            topic = (char *) zlistx_next (self->topics);
        }
    }
}

static void
s_message (struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	assert(obj);

    zmosq_server_t *self = (zmosq_server_t*) obj;
    assert (self);
    zsock_t *mqtt_writter = self->mqtt_writter;
    assert (mqtt_writter);

    zmsg_t *msg = zmsg_new ();
    zmsg_addstr (msg, message->topic);
    if (message->payload)
        zmsg_addmem (msg, message->payload, message->payloadlen);
    zmsg_send (&msg, mqtt_writter);
}

//  --------------------------------------------------------------------------
//  This is the actor which runs in its own thread.

void
zmosq_server_actor (zsock_t *pipe, void *args)
{
    s_mosquitto_init ();

    zmosq_server_t * self = zmosq_server_new (pipe, args);
    if (!self)
        return;          //  Interrupted

    //  Signal actor successfully initiated
    zsock_signal (self->pipe, 0);

    mosquitto_connect_callback_set (self->mosq, s_connect);
	mosquitto_message_callback_set (self->mosq, s_message);


    while (!self->terminated)
    {
        void *which = zpoller_wait (self->poller, -1);
        if (which == pipe)
            zmosq_server_recv_api (self);

        if (which == self->mqtt_reader) {
            zmsg_t *msg = zmsg_recv (self->mqtt_reader);
            zmsg_send (&msg, pipe);
        }
    }

    mosquitto_lib_cleanup ();
    zmosq_server_destroy (&self);
}



//  --------------------------------------------------------------------------
//  Self test of this actor.

// exit on -1
static int
s_test_handle_mosquitto (bool verbose, int port)
{
    static int child_pid = -1;

    if (child_pid > 0) {
        // we're supposed to kill mosquitto instance

        FILE *fp = NULL;
        char temp_str [1024];

        char *cmdline = zsys_sprintf ("ps aux | grep [m]osquitto | grep %d | awk '{print $2}'", port);
        fp = popen (cmdline, "r");
        zstr_free (&cmdline);
        assert (fp);

        char *frv = fgets (temp_str, sizeof (temp_str), fp);
        pclose (fp);
        assert (frv == temp_str);
        assert (atoi (temp_str) != 0);

        if (verbose)
            zsys_debug ("Stopping mosquitto broker.");
        int rv = kill (atoi (temp_str), SIGKILL);
        if (rv != 0) 
            zsys_error ("executing kill (pid = '%d', SIGKILL) failed.", atoi (temp_str));

        return 0;
    }

    int f = fork ();
    if (f == 0) {
        //  child
        char *cmdline = NULL;
        if (verbose)
            cmdline = zsys_sprintf ("mosquitto --verbose -p %d", port);
        else
            cmdline = zsys_sprintf ("mosquitto -p %d", port);

        // upstream mosquitto installs binary to /usr/sbin, which is not in the PATH for most of users
        // so add /usr/sbin/ there
        const char* PATH = getenv ("PATH");
        char* NPATH = zsys_sprintf ("/usr/sbin:%s", PATH);
        setenv ("PATH", NPATH, 1);
        zstr_free (&NPATH);

        if (verbose)
            zsys_debug ("Starting mosquitto broker: `%s`", cmdline);
        int r = system (cmdline);
        zstr_free (&cmdline);
        assert (r >= 0);
        return -1;

    }
    else
    if (f > 0) {
        child_pid = f;
    }
    else {
        zsys_error ("Failed to fork mosquitto.");
        exit (EXIT_FAILURE);
    }
    return 0;
}

void
zmosq_server_test (bool verbose)
{
    printf (" * zmosq_server:\n");
    fflush (stdout);

    int PORT = 0;
    char *PORTA = NULL;
    srand (time (NULL));
    PORT = 1024 + (rand () % 4096);

    if (s_test_handle_mosquitto (verbose, PORT) == -1)
        return;
    PORTA = zsys_sprintf ("%d", PORT);
    zclock_sleep (3000);    // helps broker to initialize

    //  @selftest
    //  Simple create/destroy test

    zactor_t *zmosq_server = zactor_new (zmosq_server_actor, NULL);
    zstr_sendx (zmosq_server, "CONNECT", "127.0.0.1", PORTA, "10", "127.0.0.1", NULL);
    zstr_sendx (zmosq_server, "SUBSCRIBE", "TEST", "TEST2", "TOPIC", "SOME MORE", NULL);
    zstr_sendx (zmosq_server, "START", NULL);

    zactor_t *zmosq_pub = zactor_new (zmosq_server_actor, NULL);
    zstr_sendx (zmosq_pub, "CONNECT", "127.0.0.1", PORTA, "10", "127.0.0.1", NULL);
    zstr_sendx (zmosq_pub, "START", NULL);
    zclock_sleep (3000); // helps actor to estabilish connection to broker

    int i = 0;

    for (i = 0; i < 20; i++) {
        zstr_sendx (zmosq_pub, "PUBLISH", (i % 2 == 0) ? "TOPIC" : "TEST", "0", "false", "HELLO, FRAME", NULL);
    }
    zclock_sleep (500);

    for (i = 0; i < 20; i++) {
        zmsg_t *msg = zmsg_recv (zmosq_server);
        assert (msg);
        char *topic, *body;
        topic = zmsg_popstr (msg);
        body = zmsg_popstr (msg);
        if (i % 2 == 0)
            assert (streq (topic, "TOPIC"));
        else
            assert (streq (topic, "TEST"));
        assert (streq (body, "HELLO, FRAME"));
        zstr_free (&topic);
        zstr_free (&body);
        zmsg_destroy (&msg);
    }
    
    zactor_destroy (&zmosq_pub);
    zactor_destroy (&zmosq_server);
    //  @end
    zstr_free (&PORTA);
    s_test_handle_mosquitto (verbose, PORT);

    printf ("OK\n");
}

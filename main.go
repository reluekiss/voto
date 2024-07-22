package main

import (
	"fmt"
	"log"
	"os"

	alsa "github.com/nathan-hello/goalsa"
) 

func main() {
    /* cmd := exec.Command("arecord", "--dump-hw-params")

    // Execute the command and capture the output
    output, err := cmd.CombinedOutput()
    if err != nil {
        fmt.Println("Error executing command:", err)
        return
    }

    // Print the output
    fmt.Println(string(output)) */
	/* // Check hardware format capabilities
	alsa.FormatS24LE
	switch 
		case :
		8, 16, 24, 32, 64
	// Check if x86
	if {
		format = alsa.FormatSXXLE
	} else {
		format = alsa.FormatSXXBE
	} */
    
    var samples []byte

    c, err := alsa.NewCaptureDevice("default", 2, alsa.FormatS24_3LE, 48000, alsa.BufferParams{})
    if err != nil {
	log.Fatal(err)
    }
    err = c.StartReadThread()
    if err != nil {
	log.Fatal(err)
    }
   
    file, err := os.OpenFile("test.wav", os.O_CREATE | os.O_RDWR,0644)
    if err != nil {
	fmt.Println("Error opening file:", err)
    }

    for {
	_, err = c.Read(samples)
    	if err != nil {
	    fmt.Println("Error reading from capture device:", err)
	    break
	}
	_, err = file.Write(samples)
    	if err != nil {
	    fmt.Println("Error writing to file:", err)
	    break
	}
    }
    file.Close()
    c.Close()
}

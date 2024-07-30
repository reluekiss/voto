package main

import (
	"bufio"
	"fmt"
	"io"
	"log"
	"os"
	"strings"

	alsa "github.com/cocoonlife/goalsa"
	opus "gopkg.in/hraban/opus.v2"
)

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
func captureaudio(reader io.Reader) ([]int16, error) {
    c, err := alsa.NewCaptureDevice("default", 2, alsa.FormatS16LE, 48000, alsa.BufferParams{})
    if err != nil {
	log.Fatal(err)
    }
    defer c.Close()

    err = c.StartReadThread()
    if err != nil {
	log.Fatal(err)
    }
    
    enc, err := opus.NewEncoder(48000, 2, opus.AppVoIP)
    if err != nil {
        log.Fatal(err)
    }
    enc.SetBitrate(48000)
    
    samples := make([]int16, 100000)
    var returnBuffer []int16
    buffer := make([]byte, 1000)
    r := bufio.NewReader(reader)
    
    for {
	input, _ := r.ReadString('\n')
        input = strings.TrimSpace(input)

        if string(input[0]) == "q" {
            break
        }

	fmt.Printf("Started recording with device %#v\n", c)
	_, err = c.Read(samples)
    	if err != nil {
	    fmt.Println("Error reading from capture device:", err)
	    break
	}

	frameSize := len(samples)
    	frameSizeMs := float32(frameSize) / 2 * 1000 / 48000 
    	switch frameSizeMs {
    	case 2.5, 5, 10, 20, 40, 60:
    	default:
    	    return nil, fmt.Errorf("illegal frame size: %d bytes (%f ms)", frameSize, frameSizeMs)
    	}

	_, err := enc.Encode(samples, buffer)
        if err != nil {
            log.Fatal(err)
        }
	returnBuffer = append(returnBuffer, samples...)
    }
    fmt.Printf("Stopped recording, len(returnBuffer)=%d\n", len(returnBuffer))
    return returnBuffer, nil
}

func playaudio(recordBuffer []int16) {
    p, err := alsa.NewPlaybackDevice("default", 2, alsa.FormatS16LE, 48000, alsa.BufferParams{})
    if err != nil {
	log.Fatal(err)
    }
    defer p.Close()
    
    dec, err := opus.NewDecoder(48000, 2)
    if err != nil {
        log.Fatal(err)
    }
    
   var buffer []byte 
   for {
   	_, err := dec.Decode(buffer, recordBuffer)
   	if err != nil {
   	    log.Fatal(err)
   	}
	p.Write(buffer)
	if err != nil {
    	    log.Fatal(err)
    	}
   } 
}

func main() {
    var samples []int16
    reader := os.Stdin
    r := bufio.NewReader(reader)
    
    for {
	input, _ := r.ReadString('\n')
        input = strings.TrimSpace(input)

        if string(input[0]) == "w" {
	    println("Started recording.")
	    s, err := captureaudio(reader)
	    if err != nil {
    	        log.Fatal(err)
    	    }
	    samples = s
	    fmt.Printf("Got samples back, len(samples)=%d\n", len(samples))
        }
    	if len(samples) != 0 {
	    fmt.Println("Playing back audio")
	    playaudio(samples)
	}
    }
}

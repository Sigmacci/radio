// const audioPlayer = document.getElementById('audioPlayer');

// function fetchAudioChunk() {
//     // fetch('http://localhost:8080', {
//     //     method: 'GET'
//     // })
//     //     .then(response => response.arrayBuffer())
//     //     .then(data => {
//     //         const audioContext = new (window.AudioContext || window.webkitAudioContext)();
//     //         audioContext.decodeAudioData(data, buffer => {
//     //             const source = audioContext.createBufferSource();
//     //             source.buffer = buffer;
//     //             source.connect(audioContext.destination);
//     //             source.start(0);
//     //         });
//     //     })
//     //     .catch(error => console.error('Error fetching audio chunk:', error));
//     fetch('http://localhost:8080', {
//         method: 'POST',
//         headers: {
//             'Content-Type': 'application/json'
//         },
//         body: JSON.stringify({ cmd: 'ACK' })  
//     }).then(response => response.text())
//         .then(data => console.log('Server Response:', data))
//         .catch(error => console.error('Error:', error));
// }

// audioPlayer.addEventListener('play', fetchAudioChunk);
// audioPlayer.addEventListener('pause', fetchAudioChunk);
// audioPlayer.addEventListener('waiting', fetchAudioChunk);
// audioPlayer.addEventListener('ended', fetchAudioChunk);

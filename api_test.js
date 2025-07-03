// api.js
const express = require('express');
const app = express();
app.use(express.json());

app.post('/api/datos', (req, res) => {
  console.log('Datos recibidos:', req.body);
  res.send({ status: 'OK' });
});

app.listen(3000, () => {
  console.log('API corriendo en http://localhost:3000');
});